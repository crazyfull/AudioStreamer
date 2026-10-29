#ifndef RAVAVA_OPUSCODEC_H
#define RAVAVA_OPUSCODEC_H

#include "opus.h"
#include "AudioTypes.h"
#include "AudioConfig.h"

namespace ravava {

// One call = one Opus packet. Packets are NOT concatenated, so each one can be
// sent as its own datagram and decoded independently by the receiver.
//
// The "into" overloads (Bytes& / Pcm& out) reuse the capacity of `out`, so after the first call they
// don't allocate: use those on the audio thread. The value-returning ones are conveniences.
class OpusCodec
{
private:
    OpusEncoder *m_pEncoder;
    OpusDecoder *m_pDecoder;
    int m_encChannels;
    int m_decChannels;
    int m_frameSamples;     // per channel

    unsigned char m_encBuf[1500];   // encoder output scratch
    Pcm m_decScratch;               // decoder output scratch (the biggest possible Opus packet)

public:
    // What an Opus packet says about itself (everything is in its first byte, "TOC"): the receiver needs no
    // side information, no matter which rate / bitrate / bandwidth / frame size the sender uses.
    struct PacketInfo {
        int frameMs = 0;        // audio duration of the whole packet in ms (whole ms only, 5..120)
        int channels = 0;       // 1 or 2 as encoded (a decoder with other channels up/down-mixes)
        int bandwidth = 0;      // OPUS_BANDWIDTH_NARROWBAND .. OPUS_BANDWIDTH_FULLBAND
    };
    // false = not something we can play (empty, corrupt, or a 2.5 ms frame which has no whole ms). No allocation.
    static bool Inspect(const uint8_t *opusPacket, size_t size, PacketInfo &info);

    OpusCodec();
    ~OpusCodec();
    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;

    // false = invalid config (AudioConfig::Validate) or Opus rejected a setting; the reason is logged
    bool InitEncoder(const AudioConfig &cfg);
    bool InitDecoder(int sampleRate, int channels);
    // what the encoder is tuned for (OPUS_APPLICATION_* / OPUS_SIGNAL_*), for tests and diagnostics. false if there is no encoder
    bool GetEncoderMode(int &application, int &signal) const;

    // pcm must hold exactly cfg.FrameSamples() * channels samples. false / empty result = error
    bool Encode(const int16_t *pcm, int frameSamples, Bytes &out);
    Bytes Encode(const int16_t *pcm, int frameSamples);

    // packet = exactly one Opus packet, gives 16bit PCM (interleaved). false = error (out is empty)
    bool Decode(const uint8_t *packet, size_t size, Pcm &out);
    bool Decode(const Bytes &packet, Pcm &out);
    Pcm Decode(const Bytes &packet);
    // recover the frame BEFORE `nextPacket` from its inband FEC data (falls back to PLC)
    bool DecodeFec(const uint8_t *nextPacket, size_t size, int frameSamples, Pcm &out);
    bool DecodeFec(const Bytes &nextPacket, int frameSamples, Pcm &out);
    Pcm DecodeFec(const Bytes &nextPacket, int frameSamples);
    // packet lost and no FEC: let Opus extrapolate the signal
    bool DecodePlc(int frameSamples, Pcm &out);
    Pcm DecodePlc(int frameSamples);
};

} // namespace ravava

#endif // RAVAVA_OPUSCODEC_H
