#ifndef RAVAVA_PACKETQUEUE_H
#define RAVAVA_PACKETQUEUE_H

// Hands datagrams from the network/GUI thread(s) to the audio thread WITHOUT the audio thread ever
// taking a lock or allocating: a ring of fixed slots, lock-free on the consumer side.
//
//   Push()  : any thread(s). Producers share a small mutex among themselves only; the audio thread never touches it.
//   Drain() : the ONE consumer (audio thread). Wait-free.
//
// When the ring is full (audio thread stalled) Push() drops the NEW datagram and returns false.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace ravava {

class PacketQueue
{
public:
    static const size_t Slots = 256;        // power of two
    static const size_t MaxSize = 1500;     // bigger datagrams are rejected (they can't be ours: Opus packet <= 1275 + header)

    PacketQueue() : m_slots(new Slot[Slots]) {}
    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    bool Push(const uint8_t *data, size_t size)
    {
        if(!data || !size || size > MaxSize)
            return false;

        std::lock_guard<std::mutex> lk(m_producers);
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if(head - tail >= Slots)
            return false;                   // full

        Slot &s = m_slots[head & (Slots - 1)];
        s.size = uint16_t(size);
        std::memcpy(s.data, data, size);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // calls f(const uint8_t *data, size_t size) for everything queued so far; returns how many
    template<class F>
    size_t Drain(F &&f)
    {
        size_t n = 0;
        size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        while(tail != head){
            const Slot &s = m_slots[tail & (Slots - 1)];
            f(s.data, size_t(s.size));
            ++tail;
            ++n;
            m_tail.store(tail, std::memory_order_release);
        }
        return n;
    }

    // only while nothing pushes and nothing drains (before the audio device starts)
    void Clear()
    {
        m_tail.store(m_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

private:
    struct Slot {
        uint16_t size = 0;
        uint8_t data[MaxSize];
    };

    std::unique_ptr<Slot[]> m_slots;
    std::mutex m_producers;                 // only between producer threads
    std::atomic<size_t> m_head{0};          // next slot to write (producers)
    std::atomic<size_t> m_tail{0};          // next slot to read  (consumer)
};

} // namespace ravava

#endif // RAVAVA_PACKETQUEUE_H
