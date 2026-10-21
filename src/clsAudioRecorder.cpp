#include "clsAudioRecorder.h"
#include "opusCodec.h"
#include "VoicePacket.h"
#include <QAudioDevice>
#include <QMediaDevices>
#include <QDebug>
#include <QByteArray>

#include <QUrl> //music

//capture audio (mic) -> opus packets
// https://doc.qt.io/qt-6/qaudiosource.html

clsAudioRecorder::clsAudioRecorder(QObject *parent) : QObject(parent)
{
    m_pRecorder = nullptr;
    m_pPCMBufferInput = nullptr;
    m_pOpus = nullptr;
    m_pConfig = nullptr;
    m_seq = 0;
    m_marker = true;
}

clsAudioRecorder::~clsAudioRecorder()
{
    if(m_pRecorder){
        m_pRecorder->stop();
        delete m_pRecorder;
        m_pRecorder = nullptr;
    }

    if(m_pOpus){
        delete m_pOpus;
        m_pOpus = nullptr;
    }
}

void clsAudioRecorder::SetConfig(AudioConfig *pConfig)
{
    m_pConfig = pConfig;
    init();
}

void clsAudioRecorder::init()
{
    if(m_pRecorder || !m_pConfig)
        return;

    m_pOpus = new opusCodec;
    if(!m_pOpus->InitEncoder(*m_pConfig))
        qWarning() << "Opus encoder init failed";

    QAudioFormat RecorderFormat;
    RecorderFormat.setChannelCount(m_pConfig->channels);
    RecorderFormat.setSampleRate(m_pConfig->inputRate);
    RecorderFormat.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice DeviceInfo = QMediaDevices::defaultAudioInput();
    if(DeviceInfo.isNull()){
        qWarning() << "No audio input device (microphone) found!";
        return;
    }

    qDebug() << "Recorder DeviceName:" << DeviceInfo.description() << "format:" << RecorderFormat;

    if(!DeviceInfo.isFormatSupported(RecorderFormat)){
        qWarning() << "Format not supported by input device, preferred is:" << DeviceInfo.preferredFormat();
        // we still try: Qt backends usually convert 48k/16bit/mono themselves
    }

    m_pRecorder = new QAudioSource(DeviceInfo, RecorderFormat, this);

    // ~4 frames of buffer: low latency but safe against small stalls
    m_pRecorder->setBufferSize(m_pConfig->frameBytes() * 4);

    if(m_pConfig->playMusic){
        LoadMusic();
    }

}

void clsAudioRecorder::onReadyRead()
{
    if(!m_pPCMBufferInput || !m_pOpus || !m_pConfig)
        return;

    //org
    m_cacheBuffer.append(m_pPCMBufferInput->readAll());

    const int frameBytes = m_pConfig->frameBytes();

    // opus needs EXACT frames (e.g. 20ms), mic chunks have arbitrary sizes -> cut them
    while(m_cacheBuffer.size() >= frameBytes){
        QByteArray encoded = m_pOpus->Encode(m_cacheBuffer.left(frameBytes));
        m_cacheBuffer.remove(0, frameBytes);

        const quint16 seq = m_seq++;      // every frame consumes a number, even silent ones

        if(encoded.isEmpty()){
            m_marker = true;
            continue;
        }

        // DTX: 1-2 byte packets mean "silence", nothing to transmit.
        // The next real packet gets the marker so the receiver knows it was silence, not loss.
        if(m_pConfig->useDtx && encoded.size() <= 2){
            m_marker = true;
            continue;
        }

        emit onReadEncodedBuffer(VoicePacket::wrap(seq, m_marker, encoded));
        m_marker = false;
    }
}

void clsAudioRecorder::Start()
{
    if(!m_pRecorder){
        qWarning() << "Recorder not ready (no microphone?)";
        return;
    }

    m_cacheBuffer.clear();
    m_marker = true;
    m_pPCMBufferInput = m_pRecorder->start();
    if(m_pPCMBufferInput){
        if(m_pConfig->playMusic){
            connect(m_pPCMBufferInput, &QIODevice::readyRead, this, &clsAudioRecorder::PlayMusic, Qt::UniqueConnection);
        }else{
            connect(m_pPCMBufferInput, &QIODevice::readyRead, this, &clsAudioRecorder::onReadyRead, Qt::UniqueConnection);
        }

    }else{
        qWarning() << "QAudioSource::start() failed, error:" << m_pRecorder->error();
    }

    ///qDebug( "opusBitrate: [%d]", m_pConfig->opusBitrate );

}

void clsAudioRecorder::Stop()
{
    if(m_pRecorder){
        m_pRecorder->stop();
        m_pPCMBufferInput = nullptr;
        m_cacheBuffer.clear();
    }
}

void clsAudioRecorder::LoadMusic(){

    QAudioFormat desiredFormat;
    desiredFormat.setChannelCount(1);
    desiredFormat.setSampleRate(48000);
    desiredFormat.setSampleFormat(QAudioFormat::Int16);

    m_pDecoder = new QAudioDecoder(this);

    // در Qt6 دیگه setSourceFilename وجود نداره؛ باید از setSource(QUrl) استفاده کنی
    m_pDecoder->setSource(QUrl::fromLocalFile(m_pConfig->musicPath));

    qDebug() <<  "---------> fromLocalFile: " << m_pConfig->musicPath;


    if(desiredFormat.sampleRate() > 0)
        m_pDecoder->setAudioFormat(desiredFormat);

    qDebug() <<  "desiredFormat() " << desiredFormat;

    connect(m_pDecoder, &QAudioDecoder::bufferReady, this, &clsAudioRecorder::OnbufferReady);
    m_pDecoder->start();
}

void clsAudioRecorder::OnbufferReady(){

    // qDebug() <<  "OnbufferReady() " << m_pDecoder->read().sampleCount();
    QAudioBuffer buff = m_pDecoder->read();

    m_musicBuffer.append(buff.constData<char>(), buff.byteCount());
}

void clsAudioRecorder::PlayMusic()
{
    if(!m_pPCMBufferInput || !m_pOpus || !m_pConfig)
        return;

    //org
    QByteArray mBuffer;
    mBuffer.append(m_pPCMBufferInput->readAll());

    const int frameBytes = m_pConfig->frameBytes();

    //org
    //m_cacheBuffer.append(mBuffer);
    m_cacheBuffer.append(m_musicBuffer.constData(), mBuffer.length());

    m_musicBuffer.remove(0, mBuffer.length() );

    // qDebug() <<  " mBuffer.length() " << mBuffer.length();


    // opus needs EXACT frames (e.g. 20ms), mic chunks have arbitrary sizes -> cut them
    while(m_cacheBuffer.size() >= frameBytes){
        QByteArray encoded = m_pOpus->Encode(m_cacheBuffer.left(frameBytes));
        m_cacheBuffer.remove(0, frameBytes);


        const quint16 seq = m_seq++;      // every frame consumes a number, even silent ones

        if(encoded.isEmpty()){
            m_marker = true;
            continue;
        }

        // DTX: 1-2 byte packets mean "silence", nothing to transmit.
        // The next real packet gets the marker so the receiver knows it was silence, not loss.
        if(m_pConfig->useDtx && encoded.size() <= 2){
            m_marker = true;
            continue;
        }

        emit onReadEncodedBuffer(VoicePacket::wrap(seq, m_marker, encoded));
        m_marker = false;
    }
}
