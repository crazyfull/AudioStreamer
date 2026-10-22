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

    // Sink buffer = 2 frames + 40ms. It is time based on purpose: with 60ms frames "4 frames"
    // would be 240ms of hidden latency. Note that Pulse/PipeWire round the requested size to
    // something that is NOT a multiple of the frame, so pump() must handle partial writes.
    m_pAudioPlayer->setBufferSize(m_pJitter->frameBytes() * 2 + m_rate * m_channels * 2 * 40 / 1000);

    m_pbufferOutput = m_pAudioPlayer->start();
    qDebug() << "Player device:" << infoPlayer.description() << PlayerFormat << "ok:" << (m_pbufferOutput != nullptr);
    if(m_pbufferOutput)
        m_pumpTimer.start();
}

// Keeps the sound card topped up. Two levels:
//  - about to run dry (< ~30ms queued): we MUST give it something, so take the next frame
//    even if that means FEC / concealment.
//  - otherwise: top up only with real, on-time audio, so we never conceal earlier than needed.
//
// IMPORTANT (Linux): PulseAudio / PipeWire don't start playing until the sink buffer reaches its
// prebuffer level, and the buffer size they give us is NOT a multiple of our frame size
// (frameMs 40/60 -> e.g. 9600 or 13440 bytes). If we only wrote whole frames, the last
// "free" bytes would never be filled, the prebuffer level never reached = permanent silence.
// So decoded frames go through m_pending and are written in whatever pieces fit.
void clsAudioPlayer::pump()
{
    if(!m_pbufferOutput || !m_pAudioPlayer || !m_pJitter)
        return;

    const qint64 urgent = qMin<qint64>(m_rate * m_channels * 2 * 30 / 1000, m_pAudioPlayer->bufferSize() / 2);

    for(;;){
        qint64 free = m_pAudioPlayer->bytesFree();
        if(free <= 0)
            break;

        if(m_pending.isEmpty()){
            qint64 buffered = m_pAudioPlayer->bufferSize() - free;
            if(buffered >= urgent && !m_pJitter->HasFrameReady())
                break;

            m_pending = m_pJitter->PopFrame();
            if(m_pending.isEmpty())
                break;
        }

        qint64 n = qMin<qint64>(free, m_pending.size());
        qint64 w = m_pbufferOutput->write(m_pending.constData(), n);
        if(w <= 0)
            break;
        m_pending.remove(0, int(w));
    }
}

void clsAudioPlayer::Play(const QByteArray &datagram){
    if(!m_pJitter)
        return;

    m_pJitter->Push(datagram);
    //qDebug() << "datagram: " << datagram.size();
    pump();
}

bool clsAudioPlayer::GetStats(JitterBuffer::Stats &stats, int &bufferFrames, int &frameMs) const
{
    if(!m_pJitter || !m_pConfig)
        return false;

    stats = m_pJitter->stats();
    bufferFrames = m_pJitter->targetDepth();
    frameMs = m_pConfig->frameMs;
    return true;
}
