#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <atomic>
#include <memory>
#include <mutex>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "audioDevice.h"
#include "jitterBuffer.h"
#include "levelMeter.h"
#include "AudioProcessor.h"
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
    std::shared_ptr<AudioProcessor> m_aec;   // shared with clsAudioRecorder (we feed it what the speaker plays)

    PacketQueue m_queue;                    // network/GUI thread(s) -> audio thread
    std::atomic<bool> m_ready;              // true once the speaker runs: Play() accepts datagrams
    std::atomic<uint32_t> m_ingestDropped;  // datagrams dropped because the queue was full

    // stats for the GUI: the audio thread publishes a copy (never blocks: try_lock), GetStats() reads it
    mutable std::mutex m_statsMutex;
    JitterBuffer::Stats m_statsCopy;
    int m_depthCopy;

    std::atomic<int> m_volume;      // 0..100, GUI -> audio thread
    float m_gain;                   // gain of the previous block (audio thread), for a click-free ramp
    LevelMeter m_level;             // loudness of what goes to the speaker (audio thread writes, anyone reads)

    Pcm m_pending;                  // decoded PCM of the current frame, not consumed by the device yet (audio thread)
    size_t m_pendingPos;
    int m_rate;
    int m_channels;

    AudioOutput m_output;           // declared last: destroyed first

    void onDeviceData(int16_t *out, uint32_t frames);
    void publishStats();
    void applyVolume(int16_t *out, uint32_t frames);

public:
    clsAudioPlayer();
    ~clsAudioPlayer();
    void SetConfig(AudioConfig *pConfig);
    // optional echo canceller, the SAME instance as the recorder's. Call BEFORE SetConfig() (it starts the speaker).
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec) { m_aec = std::move(aec); }

    void init();
    // live receive statistics (loss, FEC/PLC, buffer depth). false if the player isn't running.
    // The figures are at most ~10 ms old.
    bool GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    // loudness of the received audio, 0..100 (0 = silence, see levelMeter.h). Any thread, lock-free.
    // It is measured on the decoded PCM that is sent to the speaker, so DTX silence / lost audio read 0.
    int GetLevel() const { return m_level.level(); }        // smooth, "right now"
    int TakePeakLevel() { return m_level.takePeak(); }      // loudest since the last call (for slow GUI timers)
    // playback volume of the received audio: 0 = mute .. 100 = unchanged (default). Any thread, takes effect
    // within ~10 ms without clicks. Quadratic curve (50 = -12 dB), which sounds like an even slider. Never amplifies,
    // so it can't clip. GetLevel()/TakePeakLevel() measure BEFORE it (the level that was received).
    void SetVolume(int percent);
    int  GetVolume() const { return m_volume.load(std::memory_order_relaxed); }
    // one received datagram (header + opus packet). Never blocks.
    void Play(const uint8_t *datagram, size_t size);
    void Play(const Bytes &datagram) { Play(datagram.data(), datagram.size()); }
};

#endif // CLSAUDIOPLAYER_H
