// Built against the INSTALLED headers only (#include <ravava/...>), nothing from the repository.
#include <ravava/Recorder.h>
#include <ravava/Player.h>
#include <ravava/OpusCodec.h>
#include <ravava/Version.h>
#include <cstdio>
using namespace ravava;

int main()
{
    std::printf("RavAva %s\n", RAVAVA_VERSION_STRING);
    AudioConfig cfg;
    std::string err;
    if(!cfg.Validate(&err)){ std::printf("config: %s\n", err.c_str()); return 1; }
    OpusCodec enc, dec;
    if(!enc.InitEncoder(cfg) || !dec.InitDecoder(cfg.outputRate, cfg.channels)) return 2;
    Pcm pcm(size_t(cfg.FrameSamples()) * cfg.channels, 0);
    Bytes packet = enc.Encode(pcm.data(), cfg.FrameSamples());
    Pcm back;
    if(packet.empty() || !dec.Decode(packet, back)) return 3;
    Recorder rec; Player play;                  // the device code is linked in too (never started: no sound card in CI)
    rec.SetConfig(&cfg); (void)play;
    std::puts("consumer OK");
    return 0;
}
