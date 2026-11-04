/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_NET_PACKET_H
#define RAVAVA_NET_PACKET_H

// The envelope of EVERY UDP datagram of the app: 3 bytes in front of the payload. Network layer only:
// nothing in here (or in PacketRouter) knows what the payload means, and the audio layer never sees the envelope.
//
//   byte 0   : packet type (Packet::Type). Decides who gets the payload (audio, later other kinds of packets).
//              0 is never valid.
//   byte 1-2 : stream id, uint16 little endian: whose stream this packet belongs to. The server fills it in, so a
//              receiver knows which user is talking (conference). A plain forwarding server does not: then each client
//              uses its own random id (Packet::NewStreamId()).
//   byte 3.. : payload. For Packet::Audio that is an audio packet body, see ../VoicePacket.h
//
// NOT compatible with the old format that started directly with the audio header.

#include "../AudioTypes.h"
#include <random>

namespace ravava {

namespace Packet
{
    const int HeaderSize = 3;

    enum Type : uint8_t
    {
        Invalid = 0,
        Audio = 1,          // payload = audio packet body (../VoicePacket.h) of one stream
    };

    struct Header
    {
        uint8_t  type = Invalid;
        uint16_t streamId = 0;
    };

    // A random stream id (1..65535, never 0) for a client that was not given one. A server that only forwards datagrams does not
    // fill the id in, so without this every client sends id 0 and a receiver cannot tell the talkers apart (ConferencePlayer needs
    // the ids to be different). 16 bits: two of N clients collide with a chance of about N*N/131072 (4 clients: 0.01 %), so a server
    // that hands out ids (or a check on join) is still the better way for big rooms.
    inline uint16_t NewStreamId()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        return uint16_t(std::uniform_int_distribution<unsigned>(1, 65535)(gen));
    }

    // Writes envelope + payload into `out` (cleared first). Reusing the same `out` avoids allocations.
    inline void WrapInto(Bytes &out, const Header &h, const uint8_t *payload, size_t payloadSize)
    {
        out.clear();
        out.push_back(h.type);
        out.push_back(uint8_t(h.streamId & 0xFF));
        out.push_back(uint8_t((h.streamId >> 8) & 0xFF));
        if(payload && payloadSize)
            out.insert(out.end(), payload, payload + payloadSize);
    }

    inline void WrapInto(Bytes &out, const Header &h, const Bytes &payload)
    {
        WrapInto(out, h, payload.data(), payload.size());
    }

    // The payload is NOT copied: `payload` points into `d`, valid as long as d is.
    // false = shorter than the envelope + 1 payload byte, or type 0.
    inline bool Parse(const uint8_t *d, size_t size, Header &h, const uint8_t *&payload, size_t &payloadSize)
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

} // namespace ravava

#endif // RAVAVA_NET_PACKET_H
