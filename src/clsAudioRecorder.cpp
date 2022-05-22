#include "clsAudioRecorder.h"

#include <QDebug>

clsAudioRecorder::clsAudioRecorder(QObject *parent) : QObject(parent)
{
    //set value
    m_pPCMBufferInput = nullptr;

    int BufferLength = 9600;    //4800 2400 24000 1920
    int SimpleRate = 48000;
    int SimpleSize = 16;
    int channel = 1;


    //
    QAudioFormat RecorderFormat;
    RecorderFormat.setChannelCount(channel);
    RecorderFormat.setSampleRate(SimpleRate); //8000 Hz  11025 Hz   16000 Hz   22050 Hz   44100 Hz   48000 Hz   88200 Hz
    RecorderFormat.setSampleSize(SimpleSize);
    RecorderFormat.setCodec("audio/pcm");
    RecorderFormat.setByteOrder(QAudioFormat::LittleEndian);
    RecorderFormat.setSampleType(QAudioFormat::UnSignedInt);


    QAudioDeviceInfo DeviveInfo = QAudioDeviceInfo::defaultInputDevice();
    if (!DeviveInfo.isFormatSupported(RecorderFormat)) {
        qWarning() << "Error RecorderFormat: Format not supported, trying to use the nearest.";


        // #Returns the closest QAudioFormat to the supplied settings that the system supports.
        RecorderFormat = DeviveInfo.nearestFormat(RecorderFormat);
    }

    //recorder
    qDebug() << "Recorder DeviceName :" << DeviveInfo.deviceName();

    //
    m_pAudioRecorder = new QAudioInput(DeviveInfo, RecorderFormat, this);
    connect(m_pAudioRecorder, SIGNAL(stateChanged(QAudio::State)), this, SLOT(onRecorderStateChanged(QAudio::State)));
    connect(m_pAudioRecorder, SIGNAL(notify()), this, SLOT(onReadyForRead()));

    //set custom seeting
    //m_pAudioRecorder->setBufferSize(BufferLength);
    m_pAudioRecorder->setNotifyInterval(40);


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
