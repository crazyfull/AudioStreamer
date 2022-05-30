#include "clsAudioRecorder.h"
#include "opusCodec.h"
#include <QAudioRecorder>
#include <QAudioProbe>
#include <QAudioDecoder>
#include <QAudioEncoderSettings>
#include <QDebug>
#include <QByteArray>

#include "clsAudioPlayer.h"

//capture audio

//QAudioEncoderSettings
//QAudioRecorder
/*
 *  https://doc.qt.io/qt-5/qaudiorecorder.html
 *  https://stackoverflow.com/questions/37269182/qaudiorecorder-detect-user-not-speaking-and-stop
 *  https://doc.qt.io/qt-5/qtdatavisualization-audiolevels-example.html             Audio level meter
 *
*/


clsAudioRecorder::clsAudioRecorder(QObject *parent) : QObject(parent)
{

    //set value
    m_pProbe = nullptr;
    m_pRecorder = nullptr;
    m_pOpus = nullptr;
    m_pResampler = nullptr;


    /*
    int BufferLength = 9600;    //4800 2400 24000 1920
    int SimpleRate = 44100;
    int SimpleSize = 16;
    int channel = 1;


    //
    QAudioFormat RecorderFormat;
    RecorderFormat.setChannelCount(channel);
    RecorderFormat.setSampleRate(SimpleRate); //8000 Hz  11025 Hz   16000 Hz   22050 Hz   44100 Hz   48000 Hz   88200 Hz
    RecorderFormat.setSampleSize(SimpleSize);
    //RecorderFormat.setCodec("audio/x-opus");
    RecorderFormat.setByteOrder(QAudioFormat::LittleEndian);
    RecorderFormat.setSampleType(QAudioFormat::UnSignedInt);


    qDebug() << "RecorderFormat1: " << RecorderFormat;


    QAudioDeviceInfo DeviveInfo = QAudioDeviceInfo::defaultInputDevice();

    //RecorderFormat = DeviveInfo.nearestFormat(RecorderFormat);
    //qDebug() << "RecorderFormat2: " << RecorderFormat;

    //RecorderFormat = DeviveInfo.preferredFormat();
    //qDebug() << "RecorderFormat3: " << RecorderFormat;



    if (!DeviveInfo.isFormatSupported(RecorderFormat)) {
        qWarning() << "Error RecorderFormat: Format not supported, trying to use the nearest.";

        // #Returns the closest QAudioFormat to the supplied settings that the system supports.
        RecorderFormat = DeviveInfo.nearestFormat(RecorderFormat);
    }

    //recorder
    qDebug() << "Recorder DeviceName: " << DeviveInfo.deviceName();

    //
    m_pAudioRecorder = new QAudioInput(DeviveInfo, RecorderFormat, this);
    connect(m_pAudioRecorder, SIGNAL(stateChanged(QAudio::State)), this, SLOT(onRecorderStateChanged(QAudio::State)));
    connect(m_pAudioRecorder, SIGNAL(notify()), this, SLOT(onReadyForRead()));

    //set custom seeting
    //m_pAudioRecorder->setBufferSize(256);
    m_pAudioRecorder->setNotifyInterval(40);
*/
}

clsAudioRecorder::~clsAudioRecorder()
{
    if(m_pProbe){
        delete m_pProbe;
        m_pProbe = nullptr;
    }

    if(m_pRecorder){
        delete m_pRecorder;
        m_pRecorder = nullptr;
    }

    if(m_pOpus){
        delete m_pOpus;
        m_pOpus = nullptr;
    }

    if(m_pResampler){
        delete m_pResampler;
        m_pResampler = nullptr;
    }
}


void clsAudioRecorder::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;

    //create objects
    init();

    //set configs
    m_pResampler->SetConfig(pConfig);
    m_pOpus->SetConfig(pConfig);
}

void clsAudioRecorder::init()
{
    if(m_pProbe)
        return;

    m_pRecorder = new QAudioRecorder();
    m_pProbe = new QAudioProbe;
    m_pOpus = new opusCodec;
    m_pResampler = new Resampler;

    //
    connect(m_pProbe, SIGNAL(audioBufferProbed(QAudioBuffer)), this, SLOT(onAudioBufferProbed(QAudioBuffer)));
    m_pProbe->setSource(m_pRecorder); // Returns true, hopefully.

    /*
         * Decode MP3
        QAudioFormat desiredFormat;
        desiredFormat.setChannelCount(2);
        desiredFormat.setCodec("audio/x-raw");
        desiredFormat.setSampleType(QAudioFormat::UnSignedInt);
        desiredFormat.setSampleRate(48000);
        desiredFormat.setSampleSize(16);

        QAudioE *decoder = new QAudioDecoder(this);
        decoder->setAudioFormat(desiredFormat);
        decoder->setSourceFilename("level1.mp3");

        connect(decoder, SIGNAL(bufferReady()), this, SLOT(onReadyForRead()));
        decoder->start();
        return;
    */

    /**/
    // printAllCodecs();

    //onAudioBufferProbed:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian, codec="audio/pcm")

    QAudioEncoderSettings RecordSettings;
    //
    // audioSettings.setCodec("audio/pcm");
    RecordSettings.setBitRate(m_pConfig->bitRate);
    RecordSettings.setChannelCount(m_pConfig->channels);
    RecordSettings.setSampleRate(m_pConfig->inputRate);
    //audioSettings.setEncodingMode()
    RecordSettings.setQuality(QMultimedia::HighQuality);

    m_pRecorder->setAudioInput("default");
    m_pRecorder->setAudioSettings(RecordSettings);

    //recorder->setContainerFormat("mp3");
    //recorder->setEncodingSettings()
    //qDebug() << "containerFormat: " << recorder->containerFormat();
}

void printAllCodecs(){

    //m_pRecorder
    QAudioRecorder *recorder = new QAudioRecorder;
    QStringList codecs_list = recorder->supportedAudioCodecs();

    for( int i = 0; i < codecs_list.count(); i++)
    {
        qDebug() << codecs_list[i];
    }

}

void clsAudioRecorder::onAudioBufferProbed(const QAudioBuffer &PCMBuffer){

    qDebug() << "onAudioBufferProbed, sampleCount: " << PCMBuffer.sampleCount() << " lenth: " << PCMBuffer.byteCount() << " frameCount: " <<  PCMBuffer.frameCount() << " format: " << PCMBuffer.format(); // buffer.format();


    // QByteArray playbuff;
    m_cacheBuffer.append((const char*)PCMBuffer.constData(), PCMBuffer.byteCount());
   // qDebug() << "m_cacheBuffer: " << m_cacheBuffer.length();

    if(m_cacheBuffer.length() == ((1920*6)*m_pConfig->channels) ){ //3840*3  960 1280*3
        onReadBuffer(m_cacheBuffer);
        //playbuff = m_cacheBuffer;
        //m_pbufferOutput->write(m_Buffer.data(), m_Buffer.length());



        m_cacheBuffer.clear();
    }else{
        return;
    }



}
void clsAudioRecorder::onReadBuffer(const QByteArray &PCMBuffer){


    //qDebug() << "onReadBuffer: " << PCMBuffer.length();
    //Resample audio 48K to 16K
    QByteArray ResampleBuffer = m_pResampler->ResampleAudio(PCMBuffer);
    //qDebug() << "ResampleBuffer: " << ResampleBuffer.length();


    //compress audio
    QByteArray encodedBuffer = m_pOpus->Encode(ResampleBuffer.data(), ResampleBuffer.length());
    //qDebug() << "encodedBuffer: " << encodedBuffer.length();

    emit onReadEncodedBuffer(encodedBuffer);

    //m_player->Play( QByteArray((const char*)PCMBuffer.data(), PCMBuffer.byteCount()) );
}

/*
void clsAudioRecorder::onReadEncodedBuffer(const QByteArray &encodedBuffer){
    qDebug() << "onReadEncodedBuffer: " << encodedBuffer.length();
}
*/

void clsAudioRecorder::Start()
{
    if(m_pRecorder){
        //m_pRecorder->setVolume(qreal(1.1));
        m_pRecorder->record(); // Now we can do things like calculating levels or performing an FFT
    }
}

void clsAudioRecorder::Stop()
{
    if(m_pRecorder){
        m_pRecorder->stop();
    }
}




