#ifndef OPUSCODEC_H
#define OPUSCODEC_H

#include "opus.h"
#include <QByteArray>
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
    int m_frameBytes;

public:
    opusCodec();
    ~opusCodec();

    bool InitEncoder(const AudioConfig &cfg);
    bool InitDecoder(int sampleRate, int channels);

    // pcmFrame must be exactly cfg.frameBytes() bytes (16bit little endian PCM)
    QByteArray Encode(const QByteArray &pcmFrame);
    // packet = exactly one Opus packet, returns 16bit PCM
    QByteArray Decode(const QByteArray &packet);
    // recover the frame BEFORE `nextPacket` from its inband FEC data (falls back to PLC if it has none)
    QByteArray DecodeFec(const QByteArray &nextPacket, int frameSamples);
    // packet lost and no FEC: let Opus extrapolate the signal
    QByteArray DecodePlc(int frameSamples);
};

#endif // OPUSCODEC_H
