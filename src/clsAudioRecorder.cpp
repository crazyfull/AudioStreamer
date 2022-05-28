#include "clsAudioRecorder.h"
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
void printAllCodecs(){

    QAudioRecorder *recorder = new QAudioRecorder;

    QStringList codecs_list = recorder->supportedAudioCodecs();


    for( int i = 0; i < codecs_list.count(); i++)
    {
        qDebug() << codecs_list[i];
    }

}

void clsAudioRecorder::Stream(){

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
    printAllCodecs();

    //onAudioBufferProbed:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian, codec="audio/pcm")

    QAudioEncoderSettings audioSettings;
    //
    audioSettings.setCodec("audio/pcm");
    audioSettings.setBitRate(16);
    audioSettings.setChannelCount(1);
    audioSettings.setSampleRate(48000);
    //audioSettings.setEncodingMode()
audioSettings.setQuality(QMultimedia::HighQuality);

    QAudioRecorder *recorder = new QAudioRecorder();

    // ... configure the audio recorder (skipped)
    QAudioProbe *probe = new QAudioProbe;
    connect(probe, SIGNAL(audioBufferProbed(QAudioBuffer)), this, SLOT(onAudioBufferProbed(QAudioBuffer)));
    probe->setSource(recorder); // Returns true, hopefully.

    recorder->setAudioInput("default");
    recorder->setAudioSettings(audioSettings);

    //recorder->setContainerFormat("mp3");

    //recorder->setEncodingSettings()

    //qDebug() << "containerFormat: " << recorder->containerFormat();

    recorder->record(); // Now we can do things like calculating levels or performing an FFT

}

void clsAudioRecorder::onAudioBufferProbed(const QAudioBuffer &buffer){

    qDebug() << "onAudioBufferProbed, sampleCount: " << buffer.sampleCount() << " lenth: " << buffer.byteCount() << " frameCount: " <<  buffer.frameCount() << " format: " << buffer.format(); // buffer.format();

    //m_player->init(buffer.format());

    m_player->Play( QByteArray((const char*)buffer.data(), buffer.byteCount()) );
    // buffer.data()
}

clsAudioRecorder::clsAudioRecorder(QObject *parent) : QObject(parent)
{


    //set value
    m_pPCMBufferInput = nullptr;
    m_player = new clsAudioPlayer;

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

void clsAudioRecorder::Start()
{

    if(!m_pPCMBufferInput){
        //start recording
        m_pPCMBufferInput = m_pAudioRecorder->start();
    }
}

void clsAudioRecorder::Stop()
{
    if(m_pPCMBufferInput){
        m_pAudioRecorder->stop();
        m_pPCMBufferInput = nullptr;
    }
}

void clsAudioRecorder::onRecorderStateChanged(QAudio::State state){
    qDebug() << "onRecorderStateChanged, State: " << state;
}

void clsAudioRecorder::onReadyForRead(){

    QByteArray Buffer = m_pPCMBufferInput->readAll();
    qDebug() << "clsAudioRecorder::onReadyForRead, PCMInputBufferLenth: " << Buffer.length();
}

void clsAudioRecorder::PlusFrame(uint value){
    m_player->PlusFrame(value);
}


void clsAudioRecorder::test(){
    return;
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setSampleSize(16);
    fmt.setChannelCount(1);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::UnSignedInt);
    //fmt.setSampleType(QAudioFormat::SignedInt);
    QAudioDeviceInfo info = QAudioDeviceInfo::defaultOutputDevice();

    qDebug() << "deviceName: " << info.deviceName();
    if (!info.isFormatSupported(fmt)) {
        //printf("default format not supported try to use nearest\n");

        fmt = info.nearestFormat(fmt);
        qDebug() << "-------------------------default format not supported: " << fmt;
    }

    QAudioOutput *output = new QAudioOutput(fmt);
    connect(output, SIGNAL(stateChanged(QAudio::State)), this, SLOT(onRecorderStateChanged(QAudio::State)));
    QIODevice* io = output->start();

    qDebug() << "io: " << io;
    //printf("state:%s",output->state());
    QAudio::State stat = output->state();
    qDebug() << "stat: " << stat;
}
