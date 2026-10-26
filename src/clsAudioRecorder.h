#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <functional>
#include <memory>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "audioDevice.h"
#include "opusCodec.h"
#include "echoCanceller.h"

// Capture side: microphone (or a music file) -> opus packets. No Qt.
// The packet handler is called on the AUDIO THREAD, once per datagram:
// 3 byte header + opus packet (see VoicePacket.h). Keep it short; hand the data over to your
// own thread (the Qt GUI uses a queued invoke).
class clsAudioRecorder
{
public:
    using PacketHandler = std::function<void(const Bytes &datagram)>;

private:
    AudioConfig *m_pConfig;
    std::unique_ptr<opusCodec> m_pOpus;
    PacketHandler m_handler;
    std::shared_ptr<EchoCanceller> m_aec;   // shared with clsAudioPlayer (it feeds the speaker signal)

    Pcm m_cache;                      // mic samples that don't fill a whole opus frame yet
    uint16_t m_seq;                   // +1 per frame (also for frames skipped by DTX)
    uint8_t m_tx;                     // counts transmitted packets (loss statistics on the receiver)
    bool m_marker;                    // next packet follows a silence gap

    Pcm m_music;                      // whole music file, already at inputRate / channels
    size_t m_musicPos;

    AudioInput m_input;               // declared last: destroyed first

    void init();
    bool LoadMusic();
    void onCapture(const int16_t *pcm, uint32_t frames);   // audio thread

public:
    clsAudioRecorder();
    ~clsAudioRecorder();
    void SetConfig(AudioConfig *pConfig);
    void SetPacketHandler(PacketHandler handler) { m_handler = std::move(handler); }   // call before Start()
    // optional echo canceller, the SAME instance as the player's. Call before Start(). Ignored while playing music.
    void SetEchoCanceller(std::shared_ptr<EchoCanceller> aec) { m_aec = std::move(aec); }
    void Start();
    void Stop();
};

#endif // CLSAUDIORECORDER_H
