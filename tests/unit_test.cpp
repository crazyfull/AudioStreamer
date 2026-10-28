// Unit tests for the Qt-free audio parts that don't need a sound card or webrtc:
//   cutBlocks, PacketQueue (threads), AudioConfig::validate + opus encoder checks, JitterBuffer loss counters.
// Build (see tests/README.txt):
//   g++ -std=c++17 -O1 -pthread -Isrc tests/unit_test.cpp src/jitterBuffer.cpp src/opusCodec.cpp -lopus -o unit_test
#include "blockCutter.h"
#include "packetQueue.h"
#include "jitterBuffer.h"
#include "VoicePacket.h"
#include "net/PacketRouter.h"
#include "opusCodec.h"
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>

static int g_fail = 0;
#define CHECK(cond) do { if(!(cond)){ std::printf("  FAIL line %d: %s\n", __LINE__, #cond); g_fail++; } } while(0)

// ---------------------------------------------------------------------------------------------
static void testCutBlocks()
{
    std::printf("cutBlocks\n");
    std::mt19937 rng(7);
    for(size_t block : {1u, 7u, 160u, 480u, 2880u}){
        std::vector<int16_t> rest; rest.reserve(block);
        const size_t cap = rest.capacity();
        std::vector<int16_t> all, got;
        for(int i = 0; i < 400; i++){
            size_t n = rng() % (block * 3 + 5);          // 0 .. > 3 blocks, includes 0
            std::vector<int16_t> chunk(n);
            for(auto &v : chunk) v = int16_t(rng());
            all.insert(all.end(), chunk.begin(), chunk.end());
            cutBlocks(rest, block, chunk.data(), n, [&](const int16_t *b){ got.insert(got.end(), b, b + block); });
            CHECK(rest.size() < block);
            CHECK(rest.capacity() == cap);                // the tail buffer never reallocates
        }
        const size_t whole = all.size() / block * block;
        CHECK(got.size() == whole);
        CHECK(std::equal(got.begin(), got.end(), all.begin()));
        CHECK(rest.size() == all.size() - whole);
    }
}

// ---------------------------------------------------------------------------------------------
static void testPacketQueue()
{
    std::printf("PacketQueue\n");
    PacketQueue q;
    uint8_t big[PacketQueue::MaxSize + 1] = {0};
    CHECK(!q.push(big, sizeof(big)));                     // too big
    CHECK(!q.push(big, 0));

    // full ring drops the NEW datagram, the old ones stay in order
    for(size_t i = 0; i < PacketQueue::Slots; i++){ uint8_t v = uint8_t(i); CHECK(q.push(&v, 1)); }
    uint8_t x = 99;
    CHECK(!q.push(&x, 1));
    size_t expect = 0; bool ordered = true;
    size_t n = q.drain([&](const uint8_t *d, size_t s){ if(s != 1 || d[0] != uint8_t(expect)) ordered = false; expect++; });
    CHECK(n == PacketQueue::Slots && ordered);
    CHECK(q.drain([](const uint8_t*, size_t){}) == 0);

    // 2 producers, 1 consumer: nothing is corrupted, each producer's packets stay in order
    std::atomic<bool> go{false}, done{false};
    const int perProducer = 200000;
    std::atomic<int> accepted[2] = {{0}, {0}};
    auto producer = [&](int id){
        while(!go) {}
        uint8_t pk[12];
        for(int i = 0; i < perProducer; i++){
            pk[0] = uint8_t(id);
            uint32_t seq = uint32_t(i);
            std::memcpy(pk + 1, &seq, 4);
            std::memset(pk + 5, id * 16 + 5, 7);          // payload pattern to detect torn copies
            if(q.push(pk, sizeof(pk))) accepted[id]++;
        }
    };
    long last[2] = {-1, -1}; long received[2] = {0, 0}; bool ok = true;
    std::thread t0(producer, 0), t1(producer, 1);
    std::thread consumer([&]{
        auto f = [&](const uint8_t *d, size_t s){
            int id = d[0]; uint32_t seq; std::memcpy(&seq, d + 1, 4);
            if(s != 12 || id > 1 || long(seq) <= last[id]) ok = false;
            for(int k = 5; k < 12; k++) if(d[k] != uint8_t(id * 16 + 5)) ok = false;
            last[id] = long(seq); received[id]++;
        };
        while(!done) q.drain(f);
        q.drain(f);
    });
    go = true;
    t0.join(); t1.join(); done = true; consumer.join();
    CHECK(ok);
    CHECK(received[0] == accepted[0] && received[1] == accepted[1]);
    std::printf("  accepted %d / %d, delivered %ld / %ld\n", accepted[0].load(), accepted[1].load(), received[0], received[1]);
}

// ---------------------------------------------------------------------------------------------
static void testConfigAndEncoder()
{
    std::printf("AudioConfig::validate / opus encoder\n");
    AudioConfig ok;
    std::string err;
    CHECK(ok.validate(&err));
    { opusCodec c; CHECK(c.InitEncoder(ok)); }

    auto bad = [&](const char *name, AudioConfig c){
        std::string e;
        bool v = c.validate(&e);
        opusCodec codec;
        bool enc = codec.InitEncoder(c);
        if(v || enc || e.empty()){ std::printf("  FAIL: %s accepted (validate=%d encoder=%d)\n", name, v, enc); g_fail++; }
    };
    { AudioConfig c; c.inputRate = 44100;      bad("inputRate 44100", c); }
    { AudioConfig c; c.inputRate = 0;          bad("inputRate 0", c); }
    { AudioConfig c; c.channels = 0;           bad("channels 0", c); }
    { AudioConfig c; c.channels = 3;           bad("channels 3", c); }
    { AudioConfig c; c.frameMs = 25;           bad("frameMs 25", c); }
    { AudioConfig c; c.frameMs = 0;            bad("frameMs 0", c); }
    { AudioConfig c; c.opusBitrate = 100;      bad("bitrate 100", c); }
    { AudioConfig c; c.opusBitrate = 600000;   bad("bitrate 600000", c); }
    { AudioConfig c; c.expectedLossPerc = 101; bad("loss 101", c); }
    { AudioConfig c; c.expectedLossPerc = -1;  bad("loss -1", c); }
    { AudioConfig c; c.MaxBandwith = 3002;     bad("bandwidth 3002 (the old default)", c); }
    { AudioConfig c; c.MaxBandwith = 1100;     bad("bandwidth 1100", c); }

    // all good combinations the UI offers must still work
    for(int ms : {10, 20, 40, 60})
        for(int rate : {8000, 16000, 48000})
            for(int ch : {1, 2}){
                AudioConfig c; c.frameMs = ms; c.inputRate = rate; c.channels = ch; c.opusBitrate = 6000;
                opusCodec codec;
                if(!codec.InitEncoder(c)){ std::printf("  FAIL: valid config rejected %dms %dHz %dch\n", ms, rate, ch); g_fail++; continue; }
                Pcm pcm(size_t(c.frameSamples()) * ch, 0);
                Bytes out;
                CHECK(codec.Encode(pcm.data(), c.frameSamples(), out) && !out.empty());
                CHECK(!codec.Encode(pcm.data(), c.frameSamples() + 1, out) && out.empty());   // wrong frame size
            }
}

// ---------------------------------------------------------------------------------------------
struct JB {
    opusCodec codec;
    std::unique_ptr<JitterBuffer> jb;
    JB(){ codec.InitDecoder(48000, 1); jb = std::make_unique<JitterBuffer>(&codec, 48000, 1, 20); }
    void push(uint16_t seq, bool marker, uint8_t tx){
        Bytes payload = {0x78, 0x01, 0x02, 0x03};
        Bytes d = VoicePacket::wrap(seq, marker, tx, payload);
        jb->Push(d.data(), d.size());
    }
};

static void testJitterLoss()
{
    std::printf("JitterBuffer loss counters\n");
    {   // no loss
        JB t; for(int i = 0; i < 300; i++) t.push(uint16_t(i), i == 0, uint8_t(i & 127));
        CHECK(t.jb->stats().lost == 0 && t.jb->stats().lostEvents == 0);
    }
    {   // 300 packets in a row lost (more than 2 laps of the 7-bit counter), no DTX: must be exact
        JB t; for(int i = 0; i < 10; i++) t.push(uint16_t(i), i == 0, uint8_t(i & 127));
        t.push(uint16_t(310), false, uint8_t(310 & 127));
        CHECK(t.jb->stats().lost == 300);
        CHECK(t.jb->stats().lostEvents == 300);
    }
    {   // loss of exactly 128 (counter wraps to the same value) must not be seen as 0
        JB t; for(int i = 0; i < 10; i++) t.push(uint16_t(i), i == 0, uint8_t(i & 127));
        t.push(uint16_t(10 + 128), false, uint8_t((10 + 128) & 127));
        CHECK(t.jb->stats().lost == 128);
    }
    {   // DTX silence: sequence gap but the tx counter continues -> NOT a loss
        JB t; for(int i = 0; i < 5; i++) t.push(uint16_t(i), i == 0, uint8_t(i));
        t.push(uint16_t(205), true, 5);              // 200 silent frames skipped, marker set
        CHECK(t.jb->stats().lost == 0);
    }
    {   // silence, then the marker packet is lost and the next one arrives: exactly 1 lost, not "the whole silence"
        JB t; for(int i = 0; i < 5; i++) t.push(uint16_t(i), i == 0, uint8_t(i));
        // frames 5..14 silent (no tx), seq 15 = marker packet, tx 5 (LOST), seq 16 = tx 6 arrives
        t.push(16, false, 6);
        CHECK(t.jb->stats().lost == 1);
    }
    {   // reorder: counted lost first, then taken back. lost goes down, lostEvents does not
        JB t; t.push(1, true, 1); t.push(3, false, 3);
        CHECK(t.jb->stats().lost == 1 && t.jb->stats().lostEvents == 1);
        t.push(2, false, 2);
        CHECK(t.jb->stats().lost == 0);
        CHECK(t.jb->stats().lostEvents == 1);
        CHECK(t.jb->stats().reordered == 1);
    }
}

// ---------------------------------------------------------------------------------------------
// audio layer: the packet body is only audio header (flags + seq) + Opus packet
static void testAudioPacketBody()
{
    std::printf("VoicePacket (audio packet body)\n");
    Bytes payload = {9, 8, 7};
    Bytes d = VoicePacket::wrap(0x1234, true, 0x55, payload);
    CHECK(VoicePacket::HeaderSize == 3 && d.size() == 3 + payload.size());
    CHECK(d[0] == uint8_t(1 | (0x55 << 1)));                    // marker + tx
    CHECK(d[1] == 0x34 && d[2] == 0x12);                        // sequence, little endian
    CHECK(d[3] == 9 && d[4] == 8 && d[5] == 7);

    uint16_t seq; bool marker; uint8_t tx; Bytes p;
    CHECK(VoicePacket::parse(d.data(), d.size(), seq, marker, tx, p));
    CHECK(seq == 0x1234 && marker && tx == 0x55 && p == payload);
    CHECK(!VoicePacket::parse(d.data(), 3, seq, marker, tx, p));        // header only, no Opus byte
    CHECK(!VoicePacket::parse(nullptr, 10, seq, marker, tx, p));

    Bytes reuse; reuse.reserve(64); const uint8_t *before = reuse.data();   // reused buffer: no allocation after the first packet
    for(int i = 0; i < 100; i++) VoicePacket::wrapInto(reuse, uint16_t(i), false, uint8_t(i), payload);
    CHECK(reuse.data() == before);
}

// ---------------------------------------------------------------------------------------------
// network layer: UDP envelope (type + stream id) and the router that sends payloads to their handlers
static void testNetLayer()
{
    std::printf("net/Packet + PacketRouter\n");
    Packet::Header h; h.type = Packet::Audio; h.streamId = 0xABCD;
    Bytes body = {1, 2, 3, 4};
    Bytes d; Packet::wrapInto(d, h, body);
    CHECK(Packet::HeaderSize == 3 && d.size() == 3 + body.size());
    CHECK(d[0] == Packet::Audio);                               // packet type is the FIRST byte
    CHECK(d[1] == 0xCD && d[2] == 0xAB);                        // stream id, little endian
    CHECK(d[3] == 1 && d[6] == 4);

    Packet::Header r; const uint8_t *pl = nullptr; size_t n = 0;
    CHECK(Packet::parse(d.data(), d.size(), r, pl, n));
    CHECK(r.type == Packet::Audio && r.streamId == 0xABCD && n == 4 && pl == d.data() + 3);   // payload is not copied
    Bytes bad = d; bad[0] = Packet::Invalid;
    CHECK(!Packet::parse(bad.data(), bad.size(), r, pl, n));                                  // type 0 is never valid
    CHECK(!Packet::parse(d.data(), 3, r, pl, n));                                             // envelope only
    CHECK(!Packet::parse(nullptr, 10, r, pl, n));

    // router
    PacketRouter router;
    int audio = 0, other = 0; uint16_t lastStream = 0; Bytes lastPayload;
    router.SetHandler(Packet::Audio, [&](const Packet::Header &hdr, const uint8_t *p, size_t sz){
        audio++; lastStream = hdr.streamId; lastPayload.assign(p, p + sz);
    });
    CHECK(router.Dispatch(d));
    CHECK(audio == 1 && lastStream == 0xABCD && lastPayload == body);       // audio handler gets the payload + whose stream it is

    Packet::Header o; o.type = 7; o.streamId = 5;
    Bytes od; Packet::wrapInto(od, o, body);
    CHECK(!router.Dispatch(od));                                // no handler for type 7: not audio, never reaches the audio handler
    CHECK(audio == 1 && router.stats().unhandled == 1);

    router.SetHandler(7, [&](const Packet::Header &hdr, const uint8_t*, size_t){ other++; lastStream = hdr.streamId; });
    CHECK(router.Dispatch(od) && other == 1 && audio == 1 && lastStream == 5);   // a new type = one more handler

    router.ClearHandler(7);
    CHECK(!router.Dispatch(od) && other == 1);
    CHECK(!router.Dispatch(bad));                               // type 0
    CHECK(!router.Dispatch(d.data(), 2));                       // too short
    CHECK(router.stats().malformed == 2 && router.stats().dispatched == 2 && router.stats().unhandled == 2);

    // end to end through the layers: envelope(audio body) -> router -> jitter buffer only sees the audio body
    opusCodec codec; codec.InitDecoder(48000, 1);
    JitterBuffer jb(&codec, 48000, 1, 20);
    PacketRouter r2;
    r2.SetHandler(Packet::Audio, [&](const Packet::Header&, const uint8_t *p, size_t sz){ jb.Push(p, sz); });
    Bytes opusLike = {0x78, 1, 2, 3};
    for(uint16_t seq = 0; seq < 5; seq++){
        Bytes audioBody = VoicePacket::wrap(seq, seq == 0, uint8_t(seq), opusLike);
        Packet::Header ah; ah.type = Packet::Audio; ah.streamId = 42;
        Bytes wire; Packet::wrapInto(wire, ah, audioBody);
        r2.Dispatch(wire);
    }
    Bytes ctrl = {9, 42, 0, 1, 2, 3, 4};
    r2.Dispatch(ctrl);
    CHECK(jb.stats().received == 5 && jb.stats().lost == 0);
}

int main()
{
    testAudioPacketBody();
    testNetLayer();
    testCutBlocks();
    testPacketQueue();
    testConfigAndEncoder();
    testJitterLoss();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
