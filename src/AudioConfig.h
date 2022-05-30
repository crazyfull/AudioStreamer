#ifndef AUDIOCONFIG_H
#define AUDIOCONFIG_H

//1920*3    24000
//1280*3    16000

//16 1 chanl
//onAudioBufferProbed, sampleCount:  1920  lenth:  3840  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian,

//16 2 chanel
//onAudioBufferProbed, sampleCount:  3840  lenth:  7680  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=2, sampleType=SignedInt, byteOrder=LittleEndian,

#define FRAME_SIZE  (1280*3)  // 1280*3    //for16000


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

struct AudioConfig
{
    //opus support
    //8000 Hz  12000 Hz   16000 Hz   24000 Hz   48,000 Hz

    int inputRate = 48000;      //samplerate recording
    int outputRate = 16000;     //samplerate play
    int bitRate = 16;
    int channels = 1;
};

#endif // AUDIOCONFIG_H
