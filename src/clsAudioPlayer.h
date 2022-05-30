#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <QObject>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include "AudioConfig.h"

class clsAudioPlayer : public QObject
{
    Q_OBJECT
    QAudioOutput *m_pAudioPlayer;
    QIODevice *m_pbufferOutput;
    class opusCodec *m_pOpus;
    AudioConfig *m_pConfig;

public:
    explicit clsAudioPlayer(QObject *parent = nullptr);
    ~clsAudioPlayer();
    void SetConfig(AudioConfig *pConfig);

    void init();
    void Play(const QByteArray &encodedBuffer);
signals:

};

#endif // CLSAUDIOPLAYER_H
