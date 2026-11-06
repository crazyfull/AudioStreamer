// Link check for the iOS build: not a test that runs, it only has to LINK against libRavAva for the iOS simulator.
// It touches the parts that need the system libraries (AVFoundation / AudioToolbox through miniaudio) and Opus.
#include <ravava/Recorder.h>
#include <ravava/Player.h>
#include <ravava/AudioDevice.h>
using namespace ravava;

int main()
{
    AudioConfig cfg;
    cfg.inputRate = cfg.outputRate = 48000;
    cfg.channels = 1;
    std::string err;
    if(!cfg.Validate(&err))
        return 1;
    Recorder rec;
    Player play;
    rec.SetPacketHandler([&](const Bytes &body){ play.Play(body); });
    rec.SetConfig(&cfg);
    play.SetConfig(&cfg);
    const bool listed = !ListInputDevices().empty() || !ListOutputDevices().empty();
    OpusCodec codec;
    codec.InitEncoder(cfg);
    return listed ? 0 : 0;       // never started here: a CI machine has no audio session
}
