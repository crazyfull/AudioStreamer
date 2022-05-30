#include "opusCodec.h"

#include <QDebug>
#include <QtEndian>


/*
    NB (narrowband)             4 kHz 	8 kHz
    MB (medium-band)[note 1] 	6 kHz 	12 kHz
    WB (wideband)           	8 kHz 	16 kHz
    SWB (super-wideband)     	12 kHz 	24 kHz
    FB (fullband)[note 2]    	20 kHz 	48 kHz
*/


opusCodec::opusCodec()
{

    //https://stackoverflow.com/questions/51638654/how-to-encode-and-decode-audio-data-with-opus
    //https://windowsquestions.com/2020/10/01/qt-audio-input-output-with-opus-lib/

    //    Audio settings:
    //    Sample Rate=48000
    //    Sample Size=16
    //    Channel Count=1
    //    Byte Order=Little Endian
    //    Sample Type= UnSignedInt

    //    Encoder settings:
    //    Sample Rate=48000
    //    Channel Count=1
    //    OPUS_APPLICATION_VOIP

    //    Decoder settings:
    //    Sample Rate=48000
    //    Channel Count=1

    m_pEncoder = nullptr;
    m_pDecryptor = nullptr;



}

opusCodec::~opusCodec()
{
    if(m_pEncoder != nullptr){
        opus_encoder_destroy(m_pEncoder);
        m_pEncoder = nullptr;
    }

    if(m_pDecryptor != nullptr){
        opus_decoder_destroy(m_pDecryptor);
        m_pDecryptor = nullptr;
    }
}

void opusCodec::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;

    //
    initEncoder();
    initDecoder();
}

void opusCodec::initEncoder()
{
    if(m_pConfig){
        if(m_pEncoder)    //disable dublicate create
            return;
        //
        //opus_int32 bitrate_bps = m_Rate;
        //opus_int32 sampling_rate = m_Rate;
        int bandwidth = OPUS_AUTO;    //bandwidth =     OPUS_BANDWIDTH_NARROWBAND       OPUS_BANDWIDTH_WIDEBAND   OPUS_BANDWIDTH_MEDIUMBAND   OPUS_AUTO;
        int complexity = 10;

        int err;
        // int sampling_rates[5] = {8000, 12000, 16000, 24000, 48000};
        // int applications[3] = {OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_VOIP, OPUS_APPLICATION_RESTRICTED_LOWDELAY};


        // qDebug() << "opusCodec: sampling_rates" << sampling_rate;

        //create encoder object
        m_pEncoder = opus_encoder_create(m_pConfig->outputRate, m_pConfig->channels, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK){
            printf("Cannot opus_encoder_create: [%s]\n", opus_strerror(err));
            return;
        }



        opus_encoder_ctl(m_pEncoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));


        /*
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_BITRATE(bitrate_bps));
        opus_encoder_ctl(m_pEncoder, OPUS_SET_COMPLEXITY(complexity));
        */

        //opus_encoder_ctl(m_pEncriptor, OPUS_SET_LSB_DEPTH(16));

        /*
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_VBR(0));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_DTX(1));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_INBAND_FEC(0));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_BANDWIDTH(12000));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_PACKET_LOSS_PERC(1));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(m_pEncriptor, OPUS_SET_PACKET_LOSS_PERC(1));
    */


        //opus_encoder_ctl(enc, OPUS_SET_VBR(0));
        //opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(cvbr));
        //opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(use_inbandfec));
        //opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(forcechannels));
        //opus_encoder_ctl(enc, OPUS_SET_DTX(use_dtx));
        //opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(packet_loss_perc));
        //opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&skip));
        //opus_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_ARG));

        //opus_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(variable_duration));

        //len[toggle] = opus_encode(enc, in, frame_size, data[toggle], max_payload_bytes);
    }

}

void opusCodec::initDecoder()
{
    if(m_pConfig){
        if(m_pDecryptor)    //disable dublicate create
            return;

        int err = 0;
        m_pDecryptor = opus_decoder_create(m_pConfig->outputRate, m_pConfig->channels, &err);
        if (!m_pDecryptor) {
            qDebug() << "Failed to create OPUS decoder: " << opus_strerror(err);
        }

       // opus_decoder_ctl(m_pDecryptor, OPUS_SET_BITRATE(m_pConfig->bitRate));
    }
}

opus_int32 opusCodec::encodeFrame(const char *pcmBuffer,  char *OutPut, int OutputBufferSize) //Encoder
{
    if(m_pEncoder){
        int const OPUS_INT_SIZE = sizeof(opus_int16);
        int const frameSize = FRAME_SIZE / OPUS_INT_SIZE;

        opus_int16 input_frame[frameSize] = {0};

        //Copy the 16 bits values to float so Speex can work on them
        short *in = (short*)pcmBuffer;
        for (int i = 0; i < frameSize; i++)
        {
            input_frame[i] = (opus_int16)*in;
            in++;
        }

        //opus_int32 OutputLength =
        return opus_encode(m_pEncoder, input_frame, frameSize, (unsigned char*)OutPut, OutputBufferSize);
    }else{
        return -1;
    }
}

int opusCodec::decodeFrame(const char *EncodedBuffer, opus_int32 EncodedBufferLength, char *OutPutPCM)
{
    if(m_pDecryptor){
        int const OPUS_INT_SIZE = sizeof(opus_int16); //2
        opus_int16 output_frame[FRAME_SIZE*2] = {0}; // x2 faghat vase stereo

        int OutputLength = opus_decode(m_pDecryptor, (const unsigned char *)EncodedBuffer, EncodedBufferLength, output_frame, FRAME_SIZE, 0) * OPUS_INT_SIZE;

        opus_int16 *out = (opus_int16*)OutPutPCM;
        for (int q = 0; q < OutputLength; q++)
        {
            *out = (opus_int16)output_frame[q];
            out++;
        }

        return OutputLength;
    }else{
        return -1;
    }
}

QByteArray opusCodec::Encode(const char *PCMBuffer, int PCMBufferLength)
{
    QByteArray retFrame;
    char copressedOutput[FRAME_SIZE*2] = {};
    char *pcmFrameInput = (char*)PCMBuffer;

    int count = PCMBufferLength / (FRAME_SIZE); //??

    qDebug() << "count: " << count;

    for(int i = 0; i < count; i++){

        //memcpy(copressedOutput, Buffer + (i * FRAME_SIZE1), FRAME_SIZE1);
        pcmFrameInput + (i * FRAME_SIZE);
        //-------------------------------------------
        opus_int32 encodedOutSize = this->encodeFrame(pcmFrameInput, copressedOutput, sizeof(copressedOutput));
        //-----------------------------------------
        if(encodedOutSize == -1){
            qWarning() << "Error, encodedOutSize " << encodedOutSize;
            break;
        }

        retFrame.append(copressedOutput, encodedOutSize);
    }

    //save file
    // fwrite(BufferFrame.data(), sizeof(char), BufferFrame.length(), Filefout);
    return retFrame;
}

QByteArray opusCodec::Decode(const char *Buffer, int BufferSize)
{
    QByteArray retFrame;
    char pcmOutput[FRAME_SIZE*4] = {};//FRAME_SIZE*2
    //char *FrameInput = (char*)Buffer;
    // int frSize = 20;
    // qWarning() << "opus_packet_get_bandwidth " << opus_multistream_packet_pad()

    // int count = BufferSize / frSize; //??


    // for(int i = 0; i < count;i++){

    //memcpy(copressedOutput, Buffer + (i * FRAME_SIZE1), FRAME_SIZE1);

    //    FrameInput + (i * frSize);
    //-------------------------------------------

    int pcmOutLength = this->decodeFrame(Buffer, BufferSize, pcmOutput);
    //-----------------------------------------
    if(pcmOutLength == -1){
        qWarning() << "Error, decodeFrame " << pcmOutLength;
    }
    retFrame.append(pcmOutput, pcmOutLength);


    //  }


    return retFrame;

}
