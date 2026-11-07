/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
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
    //int bitRate = 16;
    int channels = 1;

    // ---- Opus (most compressed voice profile) ----
    int frameMs = 20;           //2.5 5 10 20 40 60 (bigger = less overhead, more latency)
    int Bitrate = 8000;         //bps
    bool useDtx = false;        //silence -> no packets
    bool useFec = true;         //inband FEC
    bool musicMode = false;     //true: Opus is tuned for music (OPUS_APPLICATION_AUDIO + OPUS_SIGNAL_MUSIC) instead of speech
                                //(OPUS_APPLICATION_VOIP + OPUS_SIGNAL_VOICE). Keeps the full bandwidth at low bitrates and the fine
                                //structure of treble. Does NOT change channels / bitrate / frame size: those stay as configured.
    // ---- devices: opaque ids from ListInputDevices() / ListOutputDevices(); empty = system default ----
    std::string inputDeviceId;      //microphone
    std::string outputDeviceId;     //speaker
    bool playMusic = false;
    std::string musicPath;      // UTF-8
    int expectedLossPerc = 20;  //tells the encoder how much redundancy to spend (0..100)
    int dredMs = 0;             //Opus 1.5+ Deep REDundancy (needs libopus built with --enable-dred and RAVAVA_ENABLE_DRED): how many ms of the
                                //PAST audio every packet carries in a neural, very compact form (0 = off, 10 ms steps, max 1000). Recovers
                                //bursts of lost packets far better than inband FEC, but it only turns on from ~32 kbps and costs bitrate and CPU.
    int maxBandwidth = 1105;     //OPUS_BANDWIDTH_FULLBAND (1101 NB .. 1105 FB). was 3002 = OPUS_SIGNAL_MUSIC, which Opus rejects as a bandwidth

    // ---- Android routing ----
    bool androidCallMode = true;//Android only. true = like a call app (WhatsApp): MODE_IN_COMMUNICATION, Bluetooth handsfree, the phone keeps
                                //the microphone and speaker together. Side effect: the system treats the phone as "in a call", other apps'
                                //notification sounds are routed with the call (quiet / on the earpiece / not heard on some phones).
                                //false = plain media mode: other apps' sounds and notifications behave normally, but there is no handsfree
                                //Bluetooth microphone (Bluetooth headphones play as "high quality" media, the phone's microphone is used).

    // ---- echo cancellation (webrtc-audio-processing, see AudioProcessor.h) ----
    bool useEchoCancel = true;  //remove what the speaker plays from the mic signal (needed when speakers + mic are used together)
    int aecNearendProtection = 0; //0..3: higher = own voice is cut less during double talk (a bit more echo may pass)
    int noiseSuppression = 0;   //0 = off, 1..4 = low .. very high (webrtc noise suppression on the mic)
    bool autoGain = false;      //webrtc AGC2: automatic mic level + limiter
    bool highPassFilter = false;//webrtc high-pass filter on the mic: cuts everything below ~100 Hz (rumble, wind, hum, handling noise, DC).
                                //Voice has nothing there. Part of the same processor as the echo canceller (the application creates it,
                                //see AudioProcessor::Options::highPassFilter), so it needs the webrtc build; works alone too. Off by default: it puts the
                                //processor into the microphone path, and the echo canceller has its own high-pass filter anyway.
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
        if(Bitrate < 500 || Bitrate > 512000)
            return fail("opusBitrate (500..512000 bps)", Bitrate);
        if(expectedLossPerc < 0 || expectedLossPerc > 100)
            return fail("expectedLossPerc (0..100)", expectedLossPerc);
        if(dredMs < 0 || dredMs > 1000)
            return fail("dredMs (0..1000)", dredMs);
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
