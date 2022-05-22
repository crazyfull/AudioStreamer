#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <QObject>
#include <QAudioInput>

class clsAudioRecorder : public QObject
{
    Q_OBJECT
private:
    class QAudioInput* m_pAudioRecorder;
    class QIODevice* m_pPCMBufferInput;


public:
    explicit clsAudioRecorder(QObject *parent = nullptr);
    void Start();
    void Stop();

signals:

private slots:
    void onRecorderStateChanged(QAudio::State state);
    void onReadyForRead();
};

#endif // CLSAUDIORECORDER_H
