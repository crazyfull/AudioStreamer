#ifndef RAVAVA_RECORDER_H
#define RAVAVA_RECORDER_H

#include <functional>
#include <memory>
#include "AudioTypes.h"
#include "AudioConfig.h"
#include "AudioDevice.h"
#include "OpusCodec.h"
#include "AudioProcessor.h"
#include "BlockCutter.h"
#include "GainRamp.h"

namespace ravava {

// Capture side: microphone (or a music file) -> opus packets. No Qt.
// The packet handler is called on the AUDIO THREAD, once per datagram:
// 3 byte audio header + opus packet (see VoicePacket.h); the network layer puts it into a UDP packet (net/Packet.h). Keep it short; hand the data over to your
// own thread (the Qt GUI uses a queued invoke).
class Recorder
{
public:
    using PacketHandler = std::function<void(const Bytes &datagram)>;

private:
    AudioConfig *m_pConfig;
    std::unique_ptr<OpusCodec> m_pOpus;
    PacketHandler m_handler;
    std::shared_ptr<AudioProcessor> m_aec;   // shared with Player (it feeds the speaker signal)

    // Audio thread rule: after Start() nothing below allocates. Everything is sized once in Start()
    // (frames are cut straight out of the device buffer, only the < 1 frame tail is kept).
    Pcm m_cache;                      // tail of mic samples that don't fill a whole opus frame yet (< 1 frame)
    Pcm m_scratch;                    // echo canceller output / music samples for the current device chunk
    Pcm m_gained;                     // mic chunk after the input gain (only used while the gain is not 100 %)
    GainRamp m_inputGain;             // microphone gain 0..200 %, GUI -> audio thread (see GainRamp.h)
    Bytes m_encoded;                  // one opus packet
    Bytes m_datagram;                 // header + opus packet, handed to the packet handler
    uint16_t m_seq;                   // +1 per frame (also for frames skipped by DTX)
    uint8_t m_tx;                     // counts transmitted packets (loss statistics on the receiver)
    bool m_marker;                    // next packet follows a silence gap

    Pcm m_music;                      // whole music file, already at inputRate / channels
    size_t m_musicPos;

    AudioInput m_input;               // declared last: destroyed first

    void init();
    bool loadMusic();
    void onCapture(const int16_t *pcm, uint32_t frames);   // audio thread
    void onFrame(const int16_t *frame);                     // audio thread: exactly one opus frame

public:
    Recorder();
    ~Recorder();
    void SetConfig(AudioConfig *pConfig);
    void SetPacketHandler(PacketHandler handler) { m_handler = std::move(handler); }   // call before Start()
    // optional echo canceller, the SAME instance as the player's. Call before Start(). Ignored while playing music.
    void SetAudioProcessor(std::shared_ptr<AudioProcessor> aec) { m_aec = std::move(aec); }
    // microphone gain in percent: 0 = mute, 100 = unchanged (default), up to 200 = +12 dB (clips instead of wrapping).
    // Any thread, takes effect within ~10 ms without clicks. Applied to the raw mic signal BEFORE the echo canceller /
    // noise suppression / AGC (like an analog mic gain). Not used while playing a music file.
    void SetInputGain(int percent) { m_inputGain.Set(percent); }
    int  GetInputGain() const { return m_inputGain.Get(); }
    static int MaxInputGain() { return 200; }
    void Start();
    void Stop();
};

} // namespace ravava

#endif // RAVAVA_RECORDER_H
