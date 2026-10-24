#ifndef VOICEPACKET_H
#define VOICEPACKET_H

#include "AudioTypes.h"

// Wire format (3 byte header + one Opus packet):
//   byte 0   : bit0    = marker (first packet after silence / DTX gap)
//              bit1-7  = tx counter: +1 (mod 128) for every packet actually TRANSMITTED
//   byte 1-2 : sequence number, uint16 little endian, +1 for EVERY frame (also DTX-skipped ones)
namespace VoicePacket
{
    const int HeaderSize = 3;
    const uint8_t FlagMarker = 0x01;
    const uint8_t TxMask = 0x7F;

    inline Bytes wrap(uint16_t seq, bool marker, uint8_t tx, const Bytes &opusPayload)
    {
        Bytes p;
        p.reserve(HeaderSize + opusPayload.size());
        p.push_back(uint8_t((marker ? FlagMarker : 0) | ((tx & TxMask) << 1)));
        p.push_back(uint8_t(seq & 0xFF));
        p.push_back(uint8_t((seq >> 8) & 0xFF));
        p.insert(p.end(), opusPayload.begin(), opusPayload.end());
        return p;
    }

    inline bool parse(const uint8_t *d, size_t size, uint16_t &seq, bool &marker, uint8_t &tx, Bytes &opusPayload)
    {
        if(!d || size <= size_t(HeaderSize))
            return false;
        marker = (d[0] & FlagMarker) != 0;
        tx = uint8_t(d[0] >> 1);
        seq = uint16_t(d[1] | (d[2] << 8));
        opusPayload.assign(d + HeaderSize, d + size);
        return true;
    }
}

#endif // VOICEPACKET_H
