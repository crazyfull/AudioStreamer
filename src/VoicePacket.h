#ifndef VOICEPACKET_H
#define VOICEPACKET_H

#include "AudioTypes.h"

// Wire format: 6 byte header + one payload (for Audio: one Opus packet). Multi-byte fields are little endian.
//
//   byte 0   : packet type (VoicePacket::Type). 0 is never valid. Receivers ignore types they don't know,
//              so new kinds of packets can be added without breaking the audio path.
//   byte 1-2 : stream id, uint16: which stream the packet belongs to (0 = the default stream)
//   byte 3   : bit0    = marker (first packet after silence / DTX gap)
//              bit1-7  = tx counter: +1 (mod 128) for every packet actually TRANSMITTED
//   byte 4-5 : sequence number, uint16, +1 for EVERY frame (also DTX-skipped ones)
//
// NOT compatible with the old 3 byte header (sender and receiver must both be updated).
namespace VoicePacket
{
    const int HeaderSize = 6;
    const uint8_t FlagMarker = 0x01;
    const uint8_t TxMask = 0x7F;

    enum Type : uint8_t
    {
        Invalid = 0,
        Audio = 1,          // payload = one Opus packet
    };

    struct Header
    {
        uint8_t  type = Audio;
        uint16_t streamId = 0;
        uint16_t seq = 0;
        bool     marker = false;
        uint8_t  tx = 0;
    };

    // Writes the datagram into `p` (cleared first). Reusing the same `p` every time means no allocation
    // after the first packet: this is what the audio thread uses.
    inline void wrapInto(Bytes &p, const Header &h, const uint8_t *payload, size_t payloadSize)
    {
        p.clear();
        p.push_back(h.type);
        p.push_back(uint8_t(h.streamId & 0xFF));
        p.push_back(uint8_t((h.streamId >> 8) & 0xFF));
        p.push_back(uint8_t((h.marker ? FlagMarker : 0) | ((h.tx & TxMask) << 1)));
        p.push_back(uint8_t(h.seq & 0xFF));
        p.push_back(uint8_t((h.seq >> 8) & 0xFF));
        if(payload && payloadSize)
            p.insert(p.end(), payload, payload + payloadSize);
    }

    inline void wrapInto(Bytes &p, const Header &h, const Bytes &payload)
    {
        wrapInto(p, h, payload.data(), payload.size());
    }

    inline Bytes wrap(const Header &h, const Bytes &payload)
    {
        Bytes p;
        p.reserve(HeaderSize + payload.size());
        wrapInto(p, h, payload);
        return p;
    }

    // Header only: the payload is NOT copied, `payload` points into `d` (valid as long as d is).
    // false = too short (a datagram needs at least one payload byte) or type 0.
    inline bool parseHeader(const uint8_t *d, size_t size, Header &h, const uint8_t *&payload, size_t &payloadSize)
    {
        if(!d || size <= size_t(HeaderSize) || d[0] == Invalid)
            return false;
        h.type = d[0];
        h.streamId = uint16_t(d[1] | (d[2] << 8));
        h.marker = (d[3] & FlagMarker) != 0;
        h.tx = uint8_t(d[3] >> 1);
        h.seq = uint16_t(d[4] | (d[5] << 8));
        payload = d + HeaderSize;
        payloadSize = size - HeaderSize;
        return true;
    }

    inline bool parse(const uint8_t *d, size_t size, Header &h, Bytes &payload)
    {
        const uint8_t *p; size_t n;
        if(!parseHeader(d, size, h, p, n))
            return false;
        payload.assign(p, p + n);
        return true;
    }
}

#endif // VOICEPACKET_H
