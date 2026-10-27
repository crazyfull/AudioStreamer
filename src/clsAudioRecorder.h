#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <functional>
#include <memory>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "audioDevice.h"
#include "opusCodec.h"
#include "AudioProcessor.h"
#include "blockCutter.h"

// Capture side: microphone (or a music file) -> opus packets. No Qt.
// The packet handler is called on the AUDIO THREAD, once per datagram:
// 6 byte header (type, stream id, flags, seq) + opus packet (see VoicePacket.h). Keep it short; hand the data over to your
// own thread (the Qt GUI uses a queued invoke).
class clsAudioRecorder
{
public:
    using PacketHandler = std::function<void(const Bytes &datagram)>;

private:
    AudioConfig *m_pConfig;
    std::unique_ptr<opusCodec> m_pOpus;
    PacketHandler m_handler;
    std::shared_ptr<AudioProcessor> m_aec;   // shared with clsAudioPlayer (it feeds the speaker signal)

    // Audio thread rule: after Start() nothing below allocates. Everything is sized once in Start()
    // (frames are cut straight out of the device buffer, only the < 1 frame tail is kept).
    Pcm m_cache;                      // tail of mic samples that don't fill a whole opus frame yet (< 1 frame)
    Pcm m_scratch;                    // echo canceller output / music samples for the current device chunk
    Bytes m_encoded;                  // one opus packet
    Bytes m_datagram;                 // header + opus packet, handed to the packet handler
    uint16_t m_seq;                   // +1 per frame (also for frames skipped by DTX)
    uint8_t m_tx;                     // counts transmitted packets (loss statistics on the receiver)
    bool m_marker;                    // next packet follows a silence gap

    Pcm m_music;                      // whole music file, already at inputRate / channels
    size_t m_musicPos;

    AudioInput m_input;               // declared last: destroyed first

    void init();
    bool LoadMusic();
    void onCapture(const int16_t *pcm, uint32_t frames);   // audio thread
    void onFrame(const int16_t *frame);                     // audio thread: exactly one opus frame

public:
    clsAudioRecorder();
    ~clsAudioRecorder();
    void SetConfig(AudioConfig *pConfig);
    void SetPacketHandler(PacketHandler handler) { m_handler = std::move(handler); }   // call before Start()
    // optional echo canceller, the SAME instance as the player's. Call before Start(). Ignored while playing music.
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec) { m_aec = std::move(aec); }
    void Start();
    void Stop();
};

#endif // CLSAUDIORECORDER_H
