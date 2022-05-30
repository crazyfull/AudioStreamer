#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <QObject>
#include <QAudioInput>
#include <QAudioBuffer>
#include "AudioConfig.h"
#include "Resampler.h"

class clsAudioRecorder : public QObject
{
    Q_OBJECT
private:
    //class QAudioInput* m_pAudioRecorder;
    //class QIODevice* m_pPCMBufferInput;
    class QAudioRecorder *m_pRecorder;
    class QAudioProbe *m_pProbe;
    class opusCodec *m_pOpus;

    AudioConfig *m_pConfig;
    Resampler *m_pResampler;
    QByteArray m_cacheBuffer;

    void init();
    void onReadBuffer(const QByteArray &PCMBuffer);
public:
    explicit clsAudioRecorder(QObject *parent = nullptr);
    ~clsAudioRecorder();
    void Start();
    void Stop();
    void SetConfig(AudioConfig *pConfig);

signals:

    void onReadEncodedBuffer(const QByteArray &encodedBuffer);

private slots:
    void onAudioBufferProbed(const QAudioBuffer &PCMBuffer);
};

#endif // CLSAUDIORECORDER_H
