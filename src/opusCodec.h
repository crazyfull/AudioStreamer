#ifndef OPUSCODEC_H
#define OPUSCODEC_H

#include "opus.h"
#include <QByteArray>
#include "SigProc_FIX.h"
#include "AudioConfig.h"

//#define FRAME_SIZE  640   //for 8000
//#define FRAME_SIZE  1280*3 //1280    // 1280 for 16000    //640 //320 //160   960     1280

class opusCodec
{
private:
    OpusEncoder *m_pEncoder;
    OpusDecoder *m_pDecryptor;
    AudioConfig *m_pConfig;

    void initEncoder();
    void initDecoder();

    opus_int32 encodeFrame(const char *pcmBuffer, char *OutPut, int OutputBufferSize);
    int decodeFrame(const char *EncodedBuffer, opus_int32 EncodedBufferLength, char *OutPutPCM);

public:
    opusCodec();
    ~opusCodec();

    QByteArray Encode(const char *PCMBuffer, int PCMBufferLength);
    QByteArray Decode(const char *Buffer, int BufferSize);
    void SetConfig(AudioConfig *pConfig);
};

#endif // OPUSCODEC_H
