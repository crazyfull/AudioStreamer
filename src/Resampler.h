#ifndef RESAMPLER_H
#define RESAMPLER_H
#include <QByteArray>
#include "SigProc_FIX.h"
#include "AudioConfig.h"

class Resampler
{
private:
    AudioConfig *m_pConfig;
    silk_resampler_state_struct *m_pResamplerState;

public:
    Resampler();
    ~Resampler();
    void SetConfig(AudioConfig *pConfig);
    QByteArray ResampleAudio(const QByteArray &inPCM);
    void init();
};

#endif // RESAMPLER_H
