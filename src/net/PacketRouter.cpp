#include "PacketRouter.h"

bool PacketRouter::Dispatch(const uint8_t *datagram, size_t size)
{
    Packet::Header hdr;
    const uint8_t *payload = nullptr;
    size_t payloadSize = 0;

    if(!Packet::parse(datagram, size, hdr, payload, payloadSize)){
        m_stats.malformed++;
        return false;
    }

    const Handler &handler = m_handlers[hdr.type];
    if(!handler){
        m_stats.unhandled++;
        return false;
    }

    handler(hdr, payload, payloadSize);
    m_stats.dispatched++;
    return true;
}
