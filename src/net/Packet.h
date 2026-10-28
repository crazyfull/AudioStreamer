#ifndef NET_PACKET_H
#define NET_PACKET_H

// The envelope of EVERY UDP datagram of the app: 3 bytes in front of the payload. Network layer only:
// nothing in here (or in PacketRouter) knows what the payload means, and the audio layer never sees the envelope.
//
//   byte 0   : packet type (Packet::Type). Decides who gets the payload (audio, later other kinds of packets).
//              0 is never valid.
//   byte 1-2 : stream id, uint16 little endian: whose stream this packet belongs to. The server fills it in, so a
//              receiver knows which user is talking (conference).
//   byte 3.. : payload. For Packet::Audio that is an audio packet body, see ../VoicePacket.h
//
// NOT compatible with the old format that started directly with the audio header.

#include "../AudioTypes.h"

namespace Packet
{
    const int HeaderSize = 3;

    enum Type : uint8_t
    {
        Invalid = 0,
        Audio = 1,          // payload = audio packet body (VoicePacket.h) of one stream
    };

    struct Header
    {
        uint8_t  type = Invalid;
        uint16_t streamId = 0;
    };

    // Writes envelope + payload into `out` (cleared first). Reusing the same `out` avoids allocations.
    inline void wrapInto(Bytes &out, const Header &h, const uint8_t *payload, size_t payloadSize)
    {
        out.clear();
        out.push_back(h.type);
        out.push_back(uint8_t(h.streamId & 0xFF));
        out.push_back(uint8_t((h.streamId >> 8) & 0xFF));
        if(payload && payloadSize)
            out.insert(out.end(), payload, payload + payloadSize);
    }

    inline void wrapInto(Bytes &out, const Header &h, const Bytes &payload)
    {
        wrapInto(out, h, payload.data(), payload.size());
    }

    // The payload is NOT copied: `payload` points into `d`, valid as long as d is.
    // false = shorter than the envelope + 1 payload byte, or type 0.
    inline bool parse(const uint8_t *d, size_t size, Header &h, const uint8_t *&payload, size_t &payloadSize)
    {
        if(!d || size <= size_t(HeaderSize) || d[0] == Invalid)
            return false;
        h.type = d[0];
        h.streamId = uint16_t(d[1] | (d[2] << 8));
        payload = d + HeaderSize;
        payloadSize = size - HeaderSize;
        return true;
    }
}

#endif // NET_PACKET_H
