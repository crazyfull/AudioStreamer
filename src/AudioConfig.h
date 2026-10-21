#ifndef AUDIOCONFIG_H
#define AUDIOCONFIG_H

//1920*3    24000
//1280*3    16000
//3840*3    48000

//16 1 chanl
//onAudioBufferProbed, sampleCount:  1920  lenth:  3840  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian,

//16 2 chanel
//onAudioBufferProbed, sampleCount:  3840  lenth:  7680  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=2, sampleType=SignedInt, byteOrder=LittleEndian,

//#define FRAME_SIZE  ((3840/3)*9)    //48000  // lenth:  9  48000,  3   16000
//#define FRAME_SIZE  ((3840/3)*3)     //16000
//#define FRAME_SIZE  (3840)
// FRAME_SIZE macro removed: it was used as both bytes and samples. Use AudioConfig::frameMs instead.

/*
16 * 2.5 = 40 (very rare)
16 * 5 = 80 (rare)
16 * 10 = 160
16 * 20 = 320
16 * 40 = 640
16 * 60 = 960
16 * 80 = 1280
16 * 120 = 1920
*/

/*
NB (narrowband)             4 kHz	8 kHz
MB (medium-band)[note 1]	6 kHz	12 kHz
WB (wideband)               8 kHz	16 kHz
SWB (super-wideband)    	12 kHz	24 kHz
FB (fullband)[note 2]   	20 kHz	48 kHz
*/
#include <QString>

struct AudioConfig
{
    //opus support
    //8000 Hz  12000 Hz   16000 Hz   24000 Hz   48,000 Hz

    int inputRate = 48000;      //samplerate recording (encoder rate)
    int outputRate = 48000;     //samplerate play (decoder rate). Opus decoder can output any supported rate
    int bitRate = 16;
    int channels = 1;

    // ---- Opus (most compressed voice profile) ----
    int frameMs = 60;           //20 //2.5 5 10 20 40 60 (bigger = less overhead, more latency)
    int opusBitrate = 800;    //8000 12000 16000 24000 48000    //bps. 6000..16000 is good for voice (FEC needs some headroom)
    bool useDtx = false;         //silence -> no packets
    bool useFec = true;         //inband FEC: each packet carries a low-rate copy of the previous frame
    bool playMusic = false;
    QString musicPath;
    int expectedLossPerc = 80;  //20  //tells the encoder how much redundancy to spend (0..100)
    int MaxBandwith = 3002;

    int frameSamples() const {
        return inputRate * frameMs / 1000; //per channel
    }

    int frameBytes() const   {
        return frameSamples() * channels * 2;   //16bit PCM
    }
};

#endif // AUDIOCONFIG_H
