#include "clsAudioPlayer.h"
#include <QMediaPlayer>

#define FRAME_SIZE 480// 480 8        480 16
#define CHAR_FRAME_SIZE (FRAME_SIZE *2)//960

#define INPUT_RATE 48000
#define OUTPUT_RATE 16000
#define SimpleSize 16

clsAudioPlayer::clsAudioPlayer(QObject *parent): QObject{parent}
{
    m_pAudioPlayer = nullptr;

    m_Opus.setChannel(1);
    m_Opus.setRate(OUTPUT_RATE);
}

void clsAudioPlayer::init(const QAudioFormat &format1){
    if(m_pAudioPlayer)
        return;

    //set player

    //onAudioBufferProbed:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian, codec="audio/pcm")

/**/
    QAudioFormat PlayerFormat;
    PlayerFormat.setSampleRate(OUTPUT_RATE); //8,000 Hz  11,025 Hz   16,000 Hz   22,050 Hz   44,100 Hz   48,000 Hz   88,200 Hz

    PlayerFormat.setChannelCount(2);
    PlayerFormat.setSampleSize(SimpleSize);
    PlayerFormat.setCodec("audio/pcm"); //pcm
    PlayerFormat.setByteOrder(QAudioFormat::LittleEndian);
    //PlayerFormat.setSampleType(QAudioFormat::UnSignedInt);
    PlayerFormat.setSampleType(QAudioFormat::SignedInt);
    //PlayerFormat.setByteOrder(QAudioFormat::BigEndian);


    QAudioDeviceInfo infoPlayer(QAudioDeviceInfo::defaultOutputDevice());
    if (!infoPlayer.isFormatSupported(PlayerFormat)) {
        qWarning() << "error: PlayerFormat defaultOutputDevice PlayerFormat not supported, trying to use the nearest.";
        //PlayerFormat = infoPlayer.nearestFormat(RecorderFormat);
        exit(0);
        return;
    }

    m_pAudioPlayer = new QAudioOutput(PlayerFormat);
    //m_pAudioPlayer->setBufferSize(960);
    //m_pAudioPlayer->setVolume(qreal(0.1));
    m_pbufferOutput = m_pAudioPlayer->start();




}

void clsAudioPlayer::Play(const QByteArray &buffer){

    QByteArray encodedBuffer = m_Opus.Encode(buffer.data(), buffer.length());
    qDebug() << "encodedBuffer: " << encodedBuffer.length();


    //decode
    QByteArray PCMoutBuffer = m_Opus.Decode(encodedBuffer.constData(), encodedBuffer.length());

    m_pbufferOutput->write(PCMoutBuffer.data(), PCMoutBuffer.length());
}

void clsAudioPlayer::PlusFrame(uint value)
{
    m_Opus.setFRAME_SIZE(value);
}
