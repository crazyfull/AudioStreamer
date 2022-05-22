#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <QObject>
#include <QAudioInput>
#include <QAudioBuffer>

class clsAudioRecorder : public QObject
{
    Q_OBJECT
private:
    class QAudioInput* m_pAudioRecorder;
    class QIODevice* m_pPCMBufferInput;
    class clsAudioPlayer *m_player;


public:
    explicit clsAudioRecorder(QObject *parent = nullptr);
    void Start();
    void Stop();
    void Stream();

    void PlusFrame(uint value);
public slots:

signals:

private slots:
    void onRecorderStateChanged(QAudio::State state);
    void onReadyForRead();
    void onAudioBufferProbed(const QAudioBuffer &buffer);
};

#endif // CLSAUDIORECORDER_H
