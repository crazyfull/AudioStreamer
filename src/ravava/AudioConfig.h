#ifndef RAVAVA_AUDIOCONFIG_H
#define RAVAVA_AUDIOCONFIG_H

#include <string>
#include <cstdio>

namespace ravava {

/*
NB (narrowband)             4 kHz   8 kHz
MB (medium-band)            6 kHz   12 kHz
WB (wideband)               8 kHz   16 kHz
SWB (super-wideband)        12 kHz  24 kHz
FB (fullband)               20 kHz  48 kHz
*/

struct AudioConfig
{
    //opus support: 8000 12000 16000 24000 48000 Hz
    int inputRate = 48000;      //samplerate recording (encoder rate)
    int outputRate = 48000;     //samplerate play (decoder rate)
    int bitRate = 16;
    int channels = 1;

    // ---- Opus (most compressed voice profile) ----
    int frameMs = 20;           //2.5 5 10 20 40 60 (bigger = less overhead, more latency)
    int opusBitrate = 8000;      //bps
    bool useDtx = false;        //silence -> no packets
    bool useFec = true;         //inband FEC
    bool playMusic = false;
    std::string musicPath;      // UTF-8
    int expectedLossPerc = 20;  //tells the encoder how much redundancy to spend (0..100)
    int maxBandwidth = 1105;     //OPUS_BANDWIDTH_FULLBAND (1101 NB .. 1105 FB). was 3002 = OPUS_SIGNAL_MUSIC, which Opus rejects as a bandwidth

    // ---- echo cancellation (webrtc-audio-processing, see AudioProcessor.h) ----
    bool useEchoCancel = true;  //remove what the speaker plays from the mic signal (needed when speakers + mic are used together)
    int aecNearendProtection = 0; //0..3: higher = own voice is cut less during double talk (a bit more echo may pass)
    int noiseSuppression = 0;   //0 = off, 1..4 = low .. very high (webrtc noise suppression on the mic)
    bool autoGain = false;      //webrtc AGC2: automatic mic level + limiter
    int aecDelayMs = 100;        //start hint: speaker output delay + mic input delay (webrtc refines it by itself)

    // sample rate of the decoder / speaker: only rates Opus supports, anything else -> 48000
    int RenderRate() const {
        return (outputRate == 8000 || outputRate == 12000 || outputRate == 16000 ||
                outputRate == 24000 || outputRate == 48000) ? outputRate : 48000;
    }

    // Rejects values Opus can't use, so a bad form entry fails at start-up with a clear message
    // instead of giving bad quality or odd behaviour later. `err` (optional) gets the reason.
    bool Validate(std::string *err = nullptr) const {
        auto fail = [&](const char *what, int v){
            if(err){
                char b[96];
                std::snprintf(b, sizeof(b), "invalid audio config: %s = %d", what, v);
                *err = b;
            }
            return false;
        };
        if(!(inputRate == 8000 || inputRate == 12000 || inputRate == 16000 || inputRate == 24000 || inputRate == 48000))
            return fail("inputRate (Opus: 8000/12000/16000/24000/48000)", inputRate);
        if(channels < 1 || channels > 2)
            return fail("channels (1 or 2)", channels);
        if(!(frameMs == 5 || frameMs == 10 || frameMs == 20 || frameMs == 40 || frameMs == 60 || frameMs == 80 || frameMs == 100 || frameMs == 120))
            return fail("frameMs (5/10/20/40/60/80/100/120)", frameMs);
        if(opusBitrate < 500 || opusBitrate > 512000)
            return fail("opusBitrate (500..512000 bps)", opusBitrate);
        if(expectedLossPerc < 0 || expectedLossPerc > 100)
            return fail("expectedLossPerc (0..100)", expectedLossPerc);
        if(maxBandwidth < 1101 || maxBandwidth > 1105)
            return fail("maxBandwidth (1101..1105)", maxBandwidth);
        return true;
    }

    int FrameSamples() const {
        return inputRate * frameMs / 1000; //per channel
    }

    int FrameBytes() const   {
        return FrameSamples() * channels * 2;   //16bit PCM
    }
};

} // namespace ravava

#endif // RAVAVA_AUDIOCONFIG_H
