#include "Resampler.h"
#include <QDebug>

Resampler::Resampler()
{
    m_pResamplerState = nullptr;

}

Resampler::~Resampler()
{
    if(m_pResamplerState){
        delete m_pResamplerState;
        m_pResamplerState = nullptr;
    }

}

void Resampler::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;

    init();
}

void Resampler::init(){
    if(!m_pResamplerState){
        m_pResamplerState = new silk_resampler_state_struct;
        int isErr = silk_resampler_init(m_pResamplerState, m_pConfig->inputRate, m_pConfig->outputRate, 1);
        if(isErr!=0){
            qDebug() << "silk_resampler_init, inputRate: " << m_pConfig->inputRate << " outputRate: " << m_pConfig->outputRate << " isErr: " << isErr;
            delete m_pResamplerState;
            m_pResamplerState = nullptr;
        }
    }
}

QByteArray Resampler::ResampleAudio(const QByteArray &inPCM)
{

    QByteArray retBuff;
    if(!m_pResamplerState){
        qDebug() << "error: reSampleState in null: ";
        return retBuff;
    }

    // opus_int16 OutPut[640] = {0};   //320
    opus_int16 OutPut[17000] = {0};
    memset(OutPut, 0, sizeof(OutPut));

    opus_int32 InputSize = inPCM.length();
    const char *pcmBuffer = inPCM.data();
    opus_int32 outPutLength = InputSize / (m_pConfig->inputRate / m_pConfig->outputRate);

    //silk_resampler, outPutLength:  5760  InputSize:  11520  ret:  0


    //char OutPutPCM[9600] = {0};
    int ret = silk_resampler(m_pResamplerState, OutPut, (opus_int16*)pcmBuffer, InputSize/2);   //test
    qDebug() << "silk_resampler, outPutLength: " << outPutLength << " InputSize: " << InputSize << " ret: " << ret;

    retBuff.append((char*)OutPut, outPutLength);

    return retBuff;
}
