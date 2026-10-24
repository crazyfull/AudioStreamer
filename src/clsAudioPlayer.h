#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <memory>
#include <mutex>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "audioDevice.h"
#include "jitterBuffer.h"

// Receive side: datagram -> JitterBuffer (reorder / FEC / PLC) -> opus decode -> speaker.
// No Qt. Play() may be called from any thread; the speaker pulls frames from the audio thread.
class clsAudioPlayer
{
    AudioConfig *m_pConfig;
    std::unique_ptr<opusCodec> m_pOpus;
    std::unique_ptr<JitterBuffer> m_pJitter;

    mutable std::mutex m_mutex;     // protects m_pJitter / m_pOpus / m_pending (GUI/network thread <-> audio thread)
    Pcm m_pending;                  // decoded PCM of the current frame, not consumed by the device yet
    size_t m_pendingPos;
    int m_rate;
    int m_channels;

    AudioOutput m_output;           // declared last: destroyed first

    void onDeviceData(int16_t *out, uint32_t frames);

public:
    clsAudioPlayer();
    ~clsAudioPlayer();
    void SetConfig(AudioConfig *pConfig);

    void init();
    // live receive statistics (loss, FEC/PLC, buffer depth). false if the player isn't running
    bool GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const;
    // one received datagram (header + opus packet)
    void Play(const uint8_t *datagram, size_t size);
    void Play(const Bytes &datagram) { Play(datagram.data(), datagram.size()); }
};

#endif // CLSAUDIOPLAYER_H
