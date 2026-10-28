#ifndef RAVAVA_NET_PACKETROUTER_H
#define RAVAVA_NET_PACKETROUTER_H

// Receive side of the network layer: every datagram that comes out of the UDP socket goes through
// Dispatch(). It reads the envelope (Packet.h) and hands the payload to the handler registered for that
// packet type, together with the stream id. So audio code only ever gets audio packets, and new kinds of
// packets are added by registering one more handler.
//
// No Qt, no sockets, no audio. Not thread safe: call SetHandler() before the first Dispatch() and use it
// from one thread (the GUI/network thread).

#include <array>
#include <functional>
#include "Packet.h"

namespace ravava {

class PacketRouter
{
public:
    // payload points into the datagram: valid only during the call, copy what you keep
    using Handler = std::function<void(const Packet::Header &hdr, const uint8_t *payload, size_t size)>;

    struct Stats
    {
        uint32_t dispatched = 0;    // handed to a handler
        uint32_t malformed = 0;     // too short / type 0
        uint32_t unhandled = 0;     // valid, but no handler for its type (a newer / unknown kind of packet)
    };

    void SetHandler(uint8_t type, Handler handler) { m_handlers[type] = std::move(handler); }
    void ClearHandler(uint8_t type) { m_handlers[type] = nullptr; }

    // true if a handler got the packet
    bool Dispatch(const uint8_t *datagram, size_t size);
    bool Dispatch(const Bytes &datagram) { return Dispatch(datagram.data(), datagram.size()); }

    const Stats &GetStats() const { return m_stats; }

private:
    std::array<Handler, 256> m_handlers;
    Stats m_stats;
};

} // namespace ravava

#endif // RAVAVA_NET_PACKETROUTER_H
