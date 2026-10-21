#include "clsAudioPlayer.h"
#include "opusCodec.h"
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioFormat>
#include <QDebug>

clsAudioPlayer::clsAudioPlayer(QObject *parent): QObject{parent}
{
    m_pAudioPlayer = nullptr;
    m_pConfig = nullptr;
    m_pbufferOutput = nullptr;
    m_pOpus = nullptr;
    m_rate = 48000;
    m_channels = 1;

    m_pJitter = nullptr;
    m_pumpTimer.setInterval(5);
    m_pumpTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pumpTimer, &QTimer::timeout, this, [this]{ pump(); });
}

clsAudioPlayer::~clsAudioPlayer()
{
    m_pumpTimer.stop();
    if(m_pAudioPlayer){
        m_pAudioPlayer->stop();
        delete m_pAudioPlayer;
        m_pAudioPlayer = nullptr;
    }

    delete m_pJitter;
    m_pJitter = nullptr;

    if(m_pOpus){
        delete m_pOpus;
        m_pOpus = nullptr;
    }
}

void clsAudioPlayer::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

static bool isOpusRate(int r)
{
    return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000;
}

void clsAudioPlayer::init(){
    if(m_pAudioPlayer || !m_pConfig)
        return;

    QAudioDevice infoPlayer = QMediaDevices::defaultAudioOutput();
    if(infoPlayer.isNull()){
        qWarning() << "No audio output device found!";
        return;
    }

    QAudioFormat PlayerFormat;
    PlayerFormat.setSampleRate(m_pConfig->outputRate);
    PlayerFormat.setChannelCount(1);
    PlayerFormat.setSampleFormat(QAudioFormat::Int16);

    if(!infoPlayer.isFormatSupported(PlayerFormat)){
        // Opus decoder can output 8/12/16/24/48k and mono/stereo from the same stream,
        // so just adapt to what the speaker wants instead of giving up.
        QAudioFormat pref = infoPlayer.preferredFormat();
        qWarning() << "PlayerFormat not supported, device preferred:" << pref;

        PlayerFormat.setSampleRate(isOpusRate(pref.sampleRate()) ? pref.sampleRate() : 48000);
        PlayerFormat.setChannelCount(1);
        if(!infoPlayer.isFormatSupported(PlayerFormat))
            PlayerFormat.setChannelCount(2);
    }

    m_rate = PlayerFormat.sampleRate();
    m_channels = PlayerFormat.channelCount();

    m_pOpus = new opusCodec;
    if(!m_pOpus->InitDecoder(m_rate, m_channels))
        return;

    m_pJitter = new JitterBuffer(m_pOpus, m_rate, m_channels, m_pConfig->frameMs);

    m_pAudioPlayer = new QAudioSink(infoPlayer, PlayerFormat, this);

    // 4 frames (80ms) of sink buffer: small hidden latency, enough to survive timer jitter
    m_pAudioPlayer->setBufferSize(m_pJitter->frameBytes() * 4); //old 6 bood

    m_pbufferOutput = m_pAudioPlayer->start();
    qDebug() << "Player device:" << infoPlayer.description() << PlayerFormat << "ok:" << (m_pbufferOutput != nullptr);
    if(m_pbufferOutput)
        m_pumpTimer.start();
}

// Keeps the sound card topped up. Two levels:
//  - below 1 frame (about to run dry): we MUST give it something, so take the next frame
//    even if that means FEC / concealment.
//  - otherwise: top up only with real, on-time audio, so we never conceal earlier than needed.
// The sink has to be filled (nearly) completely: PulseAudio / PipeWire won't start playing
// before its buffer reaches the prebuffer level, so "keep only 2 frames queued" = silence on Linux.
void clsAudioPlayer::pump()
{
    if(!m_pbufferOutput || !m_pAudioPlayer || !m_pJitter)
        return;

    const int fb = m_pJitter->frameBytes();

    for(;;){
        qint64 free = m_pAudioPlayer->bytesFree();
        if(free < fb)
            break;

        qint64 buffered = m_pAudioPlayer->bufferSize() - free;
        if(buffered >= fb && !m_pJitter->HasFrameReady())
            break;

        QByteArray pcm = m_pJitter->PopFrame();
        if(pcm.isEmpty())
            break;

        m_pbufferOutput->write(pcm.constData(), pcm.size());
    }
}

void clsAudioPlayer::Play(const QByteArray &datagram){
    if(!m_pJitter)
        return;

    m_pJitter->Push(datagram);
    //qDebug() << "datagram: " << datagram.size();
    pump();
}
