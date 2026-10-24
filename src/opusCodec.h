#ifndef OPUSCODEC_H
#define OPUSCODEC_H

#include "opus.h"
#include "AudioTypes.h"
#include "AudioConfig.h"

// One call = one Opus packet. Packets are NOT concatenated, so each one can be
// sent as its own datagram and decoded independently by the receiver.
class opusCodec
{
private:
    OpusEncoder *m_pEncoder;
    OpusDecoder *m_pDecoder;
    int m_encChannels;
    int m_decChannels;
    int m_frameSamples;     // per channel

public:
    opusCodec();
    ~opusCodec();
    opusCodec(const opusCodec&) = delete;
    opusCodec& operator=(const opusCodec&) = delete;

    bool InitEncoder(const AudioConfig &cfg);
    bool InitDecoder(int sampleRate, int channels);

    // pcm must hold exactly cfg.frameSamples() * channels samples. Empty result = error
    Bytes Encode(const int16_t *pcm, int frameSamples);
    // packet = exactly one Opus packet, returns 16bit PCM (interleaved)
    Pcm Decode(const Bytes &packet);
    // recover the frame BEFORE `nextPacket` from its inband FEC data (falls back to PLC)
    Pcm DecodeFec(const Bytes &nextPacket, int frameSamples);
    // packet lost and no FEC: let Opus extrapolate the signal
    Pcm DecodePlc(int frameSamples);
};

#endif // OPUSCODEC_H
