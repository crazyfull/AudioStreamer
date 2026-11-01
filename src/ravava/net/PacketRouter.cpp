/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "PacketRouter.h"

namespace ravava {

bool PacketRouter::Dispatch(const uint8_t *datagram, size_t size)
{
    Packet::Header hdr;
    const uint8_t *payload = nullptr;
    size_t payloadSize = 0;

    if(!Packet::Parse(datagram, size, hdr, payload, payloadSize)){
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

} // namespace ravava
