#ifndef AUDIOCONFIG_H
#define AUDIOCONFIG_H

#include <string>

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
    int frameMs = 60;           //2.5 5 10 20 40 60 (bigger = less overhead, more latency)
    int opusBitrate = 800;      //bps
    bool useDtx = false;        //silence -> no packets
    bool useFec = true;         //inband FEC
    bool playMusic = false;
    std::string musicPath;      // UTF-8
    int expectedLossPerc = 20;  //tells the encoder how much redundancy to spend (0..100)
    int MaxBandwith = 1105;     //OPUS_BANDWIDTH_FULLBAND (1101 NB .. 1105 FB). was 3002 = OPUS_SIGNAL_MUSIC, which Opus rejects as a bandwidth

    int frameSamples() const {
        return inputRate * frameMs / 1000; //per channel
    }

    int frameBytes() const   {
        return frameSamples() * channels * 2;   //16bit PCM
    }
};

#endif // AUDIOCONFIG_H
