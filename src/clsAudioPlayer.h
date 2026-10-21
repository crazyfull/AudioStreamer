#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <QObject>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QAudioSink>
#include <QTimer>
#include "AudioConfig.h"
#include "jitterBuffer.h"

class clsAudioPlayer : public QObject
{
    Q_OBJECT
    QAudioSink *m_pAudioPlayer;
    QIODevice *m_pbufferOutput;
    class opusCodec *m_pOpus;
    AudioConfig *m_pConfig;

    JitterBuffer *m_pJitter;   // reorder + loss concealment
    QTimer m_pumpTimer;        // feeds the sink with an even stream of frames
    int m_rate;
    int m_channels;

    void pump();

public:
    explicit clsAudioPlayer(QObject *parent = nullptr);
    ~clsAudioPlayer();
    void SetConfig(AudioConfig *pConfig);

    void init();
    void Play(const QByteArray &datagram);   // one received datagram (header + opus packet)
signals:

};

#endif // CLSAUDIOPLAYER_H
