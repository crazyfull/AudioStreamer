#ifndef RAVAVA_PLAYER_H
#define RAVAVA_PLAYER_H

#include <atomic>
#include <memory>
#include <mutex>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "AudioDevice.h"
#include "JitterBuffer.h"
#include "GainRamp.h"
#include "LevelMeter.h"
#include "AudioProcessor.h"
#include "PacketQueue.h"

namespace ravava {

// Receive side: datagram -> JitterBuffer (reorder / FEC / PLC) -> opus decode -> speaker.
//
// The player needs NO description of the stream: the frame duration comes from the Opus packets themselves
// (first byte, see OpusCodec::Inspect). The sender may use any frame size / bitrate / bandwidth / input rate /
// mono or stereo, and may change it while talking; the decoder always outputs the speaker's own rate and channels
// (AudioConfig::renderRate() / channels), Opus converts. Only the speaker side of the config is used here.
// No Qt. Play() may be called from any thread; the speaker pulls frames from the audio thread.
//
// Threading: Play() only copies the datagram into a lock-free queue (PacketQueue). The audio thread
// drains that queue and is the ONLY user of the jitter buffer and the Opus decoder, so it never waits
// for the network/GUI thread (no lock shared with Play()), and Play() never waits for a decode.
class Player
{
    AudioConfig *m_pConfig;

    // Decoder + jitter buffer for ONE stream format (= frame duration). Built on the network thread when a packet with a
    // new duration shows up, handed to the audio thread through m_next, and the audio thread hands the old one
    // back through m_retired: nothing is ever allocated or freed on the audio thread.
    struct Stream {
        OpusCodec codec;
        std::unique_ptr<JitterBuffer> jitter;
        int frameMs = 0;
        Stream *retiredLink = nullptr;      // m_retired list
    };
    Stream *m_cur;                          // audio thread only: the stream being played (null until the first packet)
    std::atomic<Stream*> m_next;            // network thread -> audio thread: a new stream to switch to
    std::atomic<Stream*> m_retired;         // audio thread -> network thread: old streams, freed there

    // network side (Play() callers, serialized by m_netMutex; the audio thread never touches these)
    std::mutex m_netMutex;
    int m_netFrameMs;                       // frame duration the audio thread plays / is about to play (0 = none yet)
    int m_candidateMs;                      // a different duration that is currently being seen ...
    int m_candidateCount;                   // ... this many packets in a row
    std::shared_ptr<AudioProcessor> m_aec;   // shared with Recorder (we feed it what the speaker plays)

    PacketQueue m_queue;                    // network/GUI thread(s) -> audio thread
    std::atomic<bool> m_ready;              // true once the speaker runs: Play() accepts datagrams
    std::atomic<uint32_t> m_ingestDropped;  // datagrams dropped because the queue was full

    // stats for the GUI: the audio thread publishes a copy (never blocks: try_lock), GetStats() reads it
    mutable std::mutex m_statsMutex;
    JitterBuffer::Stats m_statsCopy;
    int m_depthCopy;

    GainRamp m_volume;              // 0..100 %, GUI -> audio thread (see GainRamp.h)
    LevelMeter m_level;             // loudness of what goes to the speaker (audio thread writes, anyone reads)

    Pcm m_pending;                  // decoded PCM of the current frame, not consumed by the device yet (audio thread)
    size_t m_pendingPos;
    int m_rate;
    int m_channels;

    AudioOutput m_output;           // declared last: destroyed first

    int m_frameMsCopy;                      // frame duration of the stream behind m_statsCopy
    std::atomic<uint32_t> m_formatDropped;  // datagrams that were not valid Opus or did not match the current stream format

    void onDeviceData(int16_t *out, uint32_t frames);
    void adoptNextStream();
    void reapRetired();
    void init();                            // called by SetConfig()
    void publishStats();

public:
    Player();
    ~Player();
    void SetConfig(AudioConfig *pConfig);
    // optional echo canceller, the SAME instance as the recorder's. Call BEFORE SetConfig() (it starts the speaker).
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec) { m_aec = std::move(aec); }

    // live receive statistics (loss, FEC/PLC, buffer depth). false if the player isn't running.
    // The figures are at most ~10 ms old.
    bool GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    // loudness of the received audio, 0..100 (0 = silence, see LevelMeter.h). Any thread, lock-free.
    // It is measured on the decoded PCM that is sent to the speaker, so DTX silence / lost audio read 0.
    int GetLevel() const { return m_level.Level(); }        // smooth, "right now"
    int TakePeakLevel() { return m_level.TakePeak(); }      // loudest since the last call (for slow GUI timers)
    // playback volume of the received audio: 0 = mute .. 100 = unchanged (default). Any thread, takes effect
    // within ~10 ms without clicks. Quadratic curve (50 = -12 dB), which sounds like an even slider. Never amplifies,
    // so it can't clip. GetLevel()/TakePeakLevel() measure BEFORE it (the level that was received).
    void SetVolume(int percent) { m_volume.Set(percent); }
    int  GetVolume() const { return m_volume.Get(); }
    // one received datagram (audio header + opus packet). Never blocks the audio thread.
    void Play(const uint8_t *datagram, size_t size);
    void Play(const Bytes &datagram) { Play(datagram.data(), datagram.size()); }
};

} // namespace ravava

#endif // RAVAVA_PLAYER_H
