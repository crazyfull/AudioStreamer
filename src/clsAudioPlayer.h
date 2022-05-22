#ifndef CLSAUDIOPLAYER_H
#define CLSAUDIOPLAYER_H

#include <QObject>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include "opusCodec.h"

class clsAudioPlayer : public QObject
{
    Q_OBJECT
    QAudioOutput *m_pAudioPlayer;
    QIODevice *m_pbufferOutput;
    opusCodec m_Opus;

public:
    explicit clsAudioPlayer(QObject *parent = nullptr);

    void init(const QAudioFormat &format1);
    void Play(const QByteArray &buffer);
    void PlusFrame(uint value);
signals:

};

#endif // CLSAUDIOPLAYER_H
