#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <atomic>
#include <memory>
#include <mutex>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "audioDevice.h"
#include "jitterBuffer.h"
#include "echoCanceller.h"
#include "packetQueue.h"

// Receive side: datagram -> JitterBuffer (reorder / FEC / PLC) -> opus decode -> speaker.
// No Qt. Play() may be called from any thread; the speaker pulls frames from the audio thread.
//
// Threading: Play() only copies the datagram into a lock-free queue (PacketQueue). The audio thread
// drains that queue and is the ONLY user of the jitter buffer and the Opus decoder, so it never waits
// for the network/GUI thread (no lock shared with Play()), and Play() never waits for a decode.
class clsAudioPlayer
{
    AudioConfig *m_pConfig;
    std::unique_ptr<opusCodec> m_pOpus;                 // audio thread only (after start)
    std::unique_ptr<JitterBuffer> m_pJitter;            // audio thread only (after start)
    std::shared_ptr<EchoCanceller> m_aec;   // shared with clsAudioRecorder (we feed it what the speaker plays)

    PacketQueue m_queue;                    // network/GUI thread(s) -> audio thread
    std::atomic<bool> m_ready;              // true once the speaker runs: Play() accepts datagrams
    std::atomic<uint32_t> m_ingestDropped;  // datagrams dropped because the queue was full

    // stats for the GUI: the audio thread publishes a copy (never blocks: try_lock), GetStats() reads it
    mutable std::mutex m_statsMutex;
    JitterBuffer::Stats m_statsCopy;
    int m_depthCopy;

    Pcm m_pending;                  // decoded PCM of the current frame, not consumed by the device yet (audio thread)
    size_t m_pendingPos;
    int m_rate;
    int m_channels;

    AudioOutput m_output;           // declared last: destroyed first

    void onDeviceData(int16_t *out, uint32_t frames);
    void publishStats();

public:
    clsAudioPlayer();
    ~clsAudioPlayer();
    void SetConfig(AudioConfig *pConfig);
    // optional echo canceller, the SAME instance as the recorder's. Call BEFORE SetConfig() (it starts the speaker).
    void SetEchoCanceller(std::shared_ptr<EchoCanceller> aec) { m_aec = std::move(aec); }

    void init();
    // live receive statistics (loss, FEC/PLC, buffer depth). false if the player isn't running.
    // The figures are at most ~10 ms old.
    bool GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    // one received datagram (header + opus packet). Never blocks.
    void Play(const uint8_t *datagram, size_t size);
    void Play(const Bytes &datagram) { Play(datagram.data(), datagram.size()); }
};

#endif // CLSAUDIOPLAYER_H
