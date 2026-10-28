#ifndef RAVAVA_VOICEPACKET_H
#define RAVAVA_VOICEPACKET_H

#include "AudioTypes.h"

namespace ravava {

// Audio packet body = everything the audio layer knows about a packet: 3 byte audio header + one Opus packet.
// The UDP envelope in front of it (packet type, stream id) belongs to the network layer, see net/Packet.h:
// the audio layer never sees it.
//
// Layout (3 byte header + one Opus packet):
//   byte 0   : bit0    = marker (first packet after silence / DTX gap)
//              bit1-7  = tx counter: +1 (mod 128) for every packet actually TRANSMITTED
//   byte 1-2 : sequence number, uint16 little endian, +1 for EVERY frame (also DTX-skipped ones)
namespace VoicePacket
{
    const int HeaderSize = 3;
    const uint8_t FlagMarker = 0x01;
    const uint8_t TxMask = 0x7F;

    // Writes the datagram into `p` (cleared first). Reusing the same `p` every time means no allocation
    // after the first packet: this is what the audio thread uses.
    inline void WrapInto(Bytes &p, uint16_t seq, bool marker, uint8_t tx, const Bytes &opusPayload)
    {
        p.clear();
        p.push_back(uint8_t((marker ? FlagMarker : 0) | ((tx & TxMask) << 1)));
        p.push_back(uint8_t(seq & 0xFF));
        p.push_back(uint8_t((seq >> 8) & 0xFF));
        p.insert(p.end(), opusPayload.begin(), opusPayload.end());
    }

    inline Bytes Wrap(uint16_t seq, bool marker, uint8_t tx, const Bytes &opusPayload)
    {
        Bytes p;
        p.reserve(HeaderSize + opusPayload.size());
        WrapInto(p, seq, marker, tx, opusPayload);
        return p;
    }

    // Header only: the opus payload is NOT copied, `payload` points into `d` (valid as long as d is).
    inline bool ParseHeader(const uint8_t *d, size_t size, uint16_t &seq, bool &marker, uint8_t &tx,
                            const uint8_t *&payload, size_t &payloadSize)
    {
        if(!d || size <= size_t(HeaderSize))
            return false;
        marker = (d[0] & FlagMarker) != 0;
        tx = uint8_t(d[0] >> 1);
        seq = uint16_t(d[1] | (d[2] << 8));
        payload = d + HeaderSize;
        payloadSize = size - HeaderSize;
        return true;
    }

    inline bool Parse(const uint8_t *d, size_t size, uint16_t &seq, bool &marker, uint8_t &tx, Bytes &opusPayload)
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

} // namespace ravava

#endif // RAVAVA_VOICEPACKET_H
