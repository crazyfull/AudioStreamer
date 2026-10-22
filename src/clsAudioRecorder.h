#ifndef CLSAUDIORECORDER_H
#define CLSAUDIORECORDER_H

#include <QObject>
#include <QAudioSource>
#include <QAudioDecoder>//vase music
#include "AudioConfig.h"

class clsAudioRecorder : public QObject
{
    Q_OBJECT
private:
    QAudioSource *m_pRecorder;        // microphone
    QIODevice    *m_pPCMBufferInput;  // raw PCM stream returned by start()
    class opusCodec *m_pOpus;

    AudioConfig *m_pConfig;
    QByteArray m_cacheBuffer;
    quint16 m_seq;                    // +1 per 20ms frame (also for frames skipped by DTX)
    quint8 m_tx;                      // counts transmitted packets (loss statistics on the receiver)
    bool m_marker;                    // next packet follows a silence gap         // mic bytes that don't fill a whole opus frame yet

    QAudioDecoder *m_pDecoder;
    QByteArray m_musicBuffer;
    void init();
    void LoadMusic();
    void PlayMusic();

public:
    explicit clsAudioRecorder(QObject *parent = nullptr);
    ~clsAudioRecorder();
    void Start();
    void Stop();
    void SetConfig(AudioConfig *pConfig);

signals:
    // exactly ONE datagram per emit: 3 byte header + opus packet (see VoicePacket.h)
    void onReadEncodedBuffer(const QByteArray &encodedBuffer);

private slots:
    void onReadyRead();
    void OnbufferReady();
};

#endif // CLSAUDIORECORDER_H
