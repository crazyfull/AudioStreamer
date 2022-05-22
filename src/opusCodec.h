#ifndef OPUSCODEC_H
#define OPUSCODEC_H

#include "opus.h"
#include <QByteArray>

//#define FRAME_SIZE  640   //for 8000
#define FRAME_SIZE  1280    // 1280 for 16000    //640 //320 //160   960     1280

class opusCodec
{
private:
    OpusEncoder *m_pEncriptor;
    OpusDecoder *m_pDecryptor;
    int m_Rate;
    int Channel;


    void initEncoder();
    void initDecoder();
public:
    opusCodec();
    opus_int32 encodeFrame(const char *pcmBuffer, char *OutPut, int OutputBufferSize);
    int decodeFrame(const char *EncodedBuffer, opus_int32 EncodedBufferLength, char *OutPutPCM);
    QByteArray slot_read_audio_input(QByteArray audio_buffer);
    int Rate() const;
    void setRate(int newRate);
    int getChannel() const;
    void setChannel(int newChannel);

    QByteArray Encode(const char *PCMBuffer, int PCMBufferLength);
    QByteArray Decode(const char *Buffer, int BufferSize);
};

#endif // OPUSCODEC_H
