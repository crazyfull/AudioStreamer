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

    init();


}

void clsAudioPlayer::init(){
    if(m_pAudioPlayer)
        return;

    //set player

    //onAudioBufferProbed:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian, codec="audio/pcm")

    /**/
    QAudioFormat PlayerFormat;
    PlayerFormat.setSampleRate(OUTPUT_RATE); //8,000 Hz  11,025 Hz   16,000 Hz   22,050 Hz   44,100 Hz   48,000 Hz   88,200 Hz

    PlayerFormat.setChannelCount(1);
    PlayerFormat.setSampleSize(SimpleSize);
    PlayerFormat.setCodec("audio/pcm"); //pcm
    PlayerFormat.setByteOrder(QAudioFormat::LittleEndian);

    PlayerFormat.setSampleType(QAudioFormat::SignedInt);
    //PlayerFormat.setSampleType(QAudioFormat::UnSignedInt);
    //PlayerFormat.setByteOrder(QAudioFormat::BigEndian);


    QAudioDeviceInfo infoPlayer(QAudioDeviceInfo::defaultOutputDevice());
    if (!infoPlayer.isFormatSupported(PlayerFormat)) {
        qWarning() << "error: PlayerFormat defaultOutputDevice PlayerFormat not supported, trying to use the nearest.";
        //PlayerFormat = infoPlayer.nearestFormat(RecorderFormat);
        exit(0);
        return;
    }

    m_pAudioPlayer = new QAudioOutput(PlayerFormat);
    //m_pAudioPlayer->setBufferSize(3840);
    //m_pAudioPlayer->setNotifyInterval(50);

    //m_pAudioPlayer->setVolume(qreal(0.1));
    m_pbufferOutput = m_pAudioPlayer->start();


}

void clsAudioPlayer::Play(const QByteArray &buffer){
    /**/
    QByteArray playbuff;
    m_Buffer += buffer;

    if(m_Buffer.length() == 3840*3 ){ //960 1280*3
        playbuff = m_Buffer;
        //m_pbufferOutput->write(m_Buffer.data(), m_Buffer.length());

        qDebug() << "playbuff: " << playbuff.length();

        m_Buffer.clear();
    }else{
        return;
    }


    QByteArray ResampleBuffer = m_Opus.ResampleAudio(playbuff);
    qDebug() << "ResampleBuffer: " << ResampleBuffer.length();

    //m_pbufferOutput->write(ResampleBuffer.data(), ResampleBuffer.length());
    //return;
    //
    QByteArray encodedBuffer = m_Opus.Encode(ResampleBuffer.data(), ResampleBuffer.length());
   qDebug() << "encodedBuffer: " << encodedBuffer.length();


    //decode
    QByteArray PCMoutBuffer = m_Opus.Decode(encodedBuffer.constData(), encodedBuffer.length());
    qDebug() << "DecodedBuffer: " << PCMoutBuffer.length();

    /* win32
onAudioBufferProbed, sampleCount:  1920  lenth:  3840  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian, codec="audio/pcm")
encodedBuffer:  72
DecodedBuffer:  1280
    */



    m_pbufferOutput->write(PCMoutBuffer.data(), PCMoutBuffer.length());
}

void clsAudioPlayer::PlusFrame(uint value)
{
    m_Opus.setFRAME_SIZE(value);
}


