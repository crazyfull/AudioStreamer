/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#ifndef RAVAVA_DEVICEWATCHDOG_H
#define RAVAVA_DEVICEWATCHDOG_H

// Calls a function about once a second on its own thread (never the audio thread). Recorder / Player use it to run KeepDevice() by
// themselves, so an app that forgets the 1 s timer (or whose GUI thread is busy) still gets its device repaired after the system
// moves the route (Android: call, notification, Bluetooth), instead of silence.
//
// Stop() returns when the function is not running any more, and wakes the thread at once (no 1 s wait).

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace ravava {

class DeviceWatchdog
{
public:
    DeviceWatchdog() = default;
    ~DeviceWatchdog() { Stop(); }
    DeviceWatchdog(const DeviceWatchdog&) = delete;
    DeviceWatchdog& operator=(const DeviceWatchdog&) = delete;

    void Start(std::function<void()> tick, int periodMs = 1000)
    {
        Stop();
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_stop = false;
        }
        m_thread = std::thread([this, tick = std::move(tick), periodMs]{
            std::unique_lock<std::mutex> lk(m_mx);
            while(!m_stop){
                if(m_cv.wait_for(lk, std::chrono::milliseconds(periodMs), [this]{ return m_stop; }))
                    break;
                lk.unlock();
                tick();
                lk.lock();
            }
        });
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_stop = true;
        }
        m_cv.notify_all();
        if(m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            m_thread.join();
    }

private:
    std::thread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    bool m_stop = true;
};

} // namespace ravava

#endif // RAVAVA_DEVICEWATCHDOG_H
