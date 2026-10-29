#ifndef RAVAVA_DEVICESWITCHER_H
#define RAVAVA_DEVICESWITCHER_H

// Runs "close the sound device and open another one" OFF the caller's thread.
//
// Closing / opening a sound device can take from tens of ms to seconds (Bluetooth headsets, USB cards: on Android the
// Bluetooth link has to come up first) and was done on the GUI thread, which froze the app (and on a phone Android kills
// an app that blocks its UI thread for ~5 s). Now the GUI only calls Request(), which returns at once.
//
//   Request(id, fn) : fn(id) runs on a worker thread. Requests that arrive while fn is running are merged: only the LAST
//                     id is applied afterwards (moving through a combo box with the mouse wheel opens one device, not ten).
//   Cancel()        : drops a request that has not started yet and waits for the one that is running. After it returns no
//                     fn is running and none will start. Call it before Stop() / Start() / destruction.
// Thread-safe: Request / Cancel can be called from any thread (but not from fn itself).

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ravava {

class DeviceSwitcher
{
public:
    using Apply = std::function<void(const std::string &deviceId)>;

    ~DeviceSwitcher() { Cancel(); }

    void Request(const std::string &deviceId, Apply fn)
    {
        std::thread old;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending = deviceId;
            m_hasPending = true;
            m_fn = std::move(fn);
            if(m_running)
                return;                         // the worker will pick it up
            old = std::move(m_thread);          // a finished worker, joined below (outside the lock)
            m_running = true;
            m_thread = std::thread([this]{ run(); });
        }
        if(old.joinable())
            old.join();
    }

    void Cancel()
    {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_hasPending = false;
            t = std::move(m_thread);
        }
        if(t.joinable())
            t.join();                           // the worker finishes the request it is in, then sees no more
    }

private:
    void run()
    {
        for(;;){
            std::string id;
            Apply fn;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if(!m_hasPending){
                    m_running = false;
                    return;
                }
                id = m_pending;
                fn = m_fn;
                m_hasPending = false;
            }
            fn(id);
        }
    }

    std::mutex m_mutex;
    std::thread m_thread;
    std::string m_pending;
    Apply m_fn;
    bool m_hasPending = false;
    bool m_running = false;
};

} // namespace ravava

#endif // RAVAVA_DEVICESWITCHER_H
