#ifndef OPUSCODEC_H
#define OPUSCODEC_H

#include "opus.h"
#include "AudioTypes.h"
#include "AudioConfig.h"

// One call = one Opus packet. Packets are NOT concatenated, so each one can be
// sent as its own datagram and decoded independently by the receiver.
//
// The "into" overloads (Bytes& / Pcm& out) reuse the capacity of `out`, so after the first call they
// don't allocate: use those on the audio thread. The value-returning ones are conveniences.
class opusCodec
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
    opusCodec();
    ~opusCodec();
    opusCodec(const opusCodec&) = delete;
    opusCodec& operator=(const opusCodec&) = delete;

    // false = invalid config (AudioConfig::validate) or Opus rejected a setting; the reason is logged
    bool InitEncoder(const AudioConfig &cfg);
    bool InitDecoder(int sampleRate, int channels);

    // pcm must hold exactly cfg.frameSamples() * channels samples. false / empty result = error
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

#endif // OPUSCODEC_H
