#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/clsAudioRecorder.h"
#include "src/clsAudioPlayer.h"
#include <QDebug>

MainWindow::MainWindow(QWidget *parent): QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_AudioConfig.bitRate = 16;
    m_AudioConfig.channels = 1; //3840 1, 7680 2
    m_AudioConfig.inputRate = 48000;
    m_AudioConfig.outputRate = 16000;//24000;//22050;//16000;

    //onAudioBufferProbed, sampleCount:  1920  lenth:  3840  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian,
    //onAudioBufferProbed, sampleCount:  1920  lenth:  3840  frameCount:  1920  format:  QAudioFormat(48000Hz, 16bit, channelCount=1, sampleType=SignedInt, byteOrder=LittleEndian,

    //outPutLength:  5760  InputSize:  11520    22050
    //outPutLength:  3840  InputSize:  11520    16000

    m_pAudioRecorder = new clsAudioRecorder(this);
    m_pAudioPlayer = new clsAudioPlayer(this);
    m_pAudioRecorder->SetConfig(&m_AudioConfig);
    m_pAudioPlayer->SetConfig(&m_AudioConfig);

    connect(m_pAudioRecorder, SIGNAL(onReadEncodedBuffer(QByteArray)), this, SLOT(onReadEncodedBuffer(QByteArray)));
}

MainWindow::~MainWindow()
{
    delete ui;
}


void MainWindow::on_btnStartRecording_clicked()
{
    m_pAudioRecorder->Start();
}


void MainWindow::on_btnStopRecording_clicked()
{
    m_pAudioRecorder->Stop();
}


void MainWindow::on_btnPlusFrame_clicked()
{
    //ui->spinBox.v += 2;
    ui->spinBox->setValue( ui->spinBox->value() + 2);

}


void MainWindow::on_spinBox_valueChanged(int arg1)
{
    //m_pAudioRecorder->PlusFrame(ui->spinBox->value() );
}

void MainWindow::onReadEncodedBuffer(const QByteArray &encodedBuffer)
{
    qDebug() << "onReadEncodedBuffer: " << encodedBuffer.length();
    m_pAudioPlayer->Play(encodedBuffer);
}
