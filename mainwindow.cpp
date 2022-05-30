#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/clsAudioRecorder.h"
#include "src/clsAudioPlayer.h"
#include <QDebug>
#include <QUdpSocket>

MainWindow::MainWindow(QWidget *parent): QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    //udp connection object
    m_pUDPClient = new QUdpSocket(this);
    connect(m_pUDPClient, SIGNAL(readyRead()),  this, SLOT(onReceiveUDPData()));
    //bool ret = m_pUDPClient->bind(QHostAddress::AnyIPv4);
    //bool ret = m_pUDPClient->bind(QHostAddress("ws.mojz.ir") , 10001); //QHostAddress("51.75.56.25"),

    //qDebug() << "UDP bind?: " << ret;


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

    //
    CreateTimer();

    ui->statusbar->showMessage("idle");
}

MainWindow::~MainWindow()
{
    delete ui;
}

QString MainWindow::GetFormatSize(qint64 Size)
{
    const int kb = 1000;
    const int mb = 1000 * kb;
    const int gb = 1000 * mb;

    if(Size >= gb )
    {
        //return QFileSystemModel::tr("%1 GB").arg(QLocale().toString(qreal(Size) / gb, 'f', 3));
        return QString::number( qreal(Size) / gb, 'f', 3 ) + " GB";
    }

    if(Size >= mb )
    {
        //return QFileSystemModel::tr("%1 MB").arg(QLocale().toString(qreal(Size) / mb, 'f', 2));
        return QString::number( qreal(Size) / mb, 'f', 3 ) + " MB";
    }

    if(Size >= kb)
    {
        //return QFileSystemModel::tr("%1 KB").arg(QLocale().toString(Size / kb));
        return QString::number( qreal(Size) / kb, 'f', 2 ) + " KB";
    }

    if(Size < kb)
    {
        return QString::number( Size  ) + " bytes";
    }


    return nullptr;
}

void MainWindow::CreateTimer()
{
    connect(&m_RateTimer, SIGNAL(timeout()), this, SLOT(OnRateTimer()));
    m_RateTimer.setSingleShot(false);
    m_RateTimer.setInterval(5*1000);
    m_RateTimer.start();
    m_Rate = 0;
    ui->statusbar->showMessage("0");
}

void MainWindow::on_btnStartRecording_clicked()
{
    m_pAudioRecorder->Start();
}


void MainWindow::on_btnStopRecording_clicked()
{
    m_pAudioRecorder->Stop();
}


void MainWindow::on_spinBox_valueChanged(int arg1)
{
    //m_pAudioRecorder->PlusFrame(ui->spinBox->value() );
}

void MainWindow::onReadEncodedBuffer(const QByteArray &encodedBuffer)
{
    SendUDP(encodedBuffer);

   // qDebug() << "onReadEncodedBuffer: " << encodedBuffer.length();

   // m_pAudioPlayer->Play(encodedBuffer);
}

void MainWindow::onReceiveUDPData(){

    QByteArray encodedBuffer;
    encodedBuffer.resize(m_pUDPClient->pendingDatagramSize());

    QHostAddress sender;
    quint16 senderPort;
    m_pUDPClient->readDatagram(encodedBuffer.data(), encodedBuffer.size(), &sender ,&senderPort );


    //QHostAddress("51.75.56.25")
    qDebug() << "onReceiveData port: " << senderPort << "Bufflength: " << encodedBuffer.length();

    if(m_pAudioRecorder == nullptr){
        qDebug() << "UDP onReceiveData();";
        return;
    }

    if(encodedBuffer.isEmpty())
        return;

    //QByteArray PCMoutBuffer = m_Opus.Decode(datagram.data() +m_DNSSize, datagram.length()-m_DNSSize);
    //qDebug() << "onReceiveData: PCMoutBuffer: " << PCMoutBuffer.length();
    // m_pbufferOutput->write(PCMoutBuffer);

    //m_Rate += encodedBuffer.length();       //for get transfer rate monitor

    m_pAudioPlayer->Play(encodedBuffer);



    /*
    //return;
    char decBuff[9600];
    memset(decBuff, 0, sizeof(decBuff));
    int DeSize = m_Speex->Decompress(datagram.data(), datagram.length(), decBuff, 9600);

    //ui->txtBuffer->setPlainText( QString::number(cmpSize*10) + " KB/sec");

    m_pbufferOutput->write(decBuff, DeSize*2);
    */
}

void MainWindow::OnRateTimer()
{

    QByteArray msg;
    msg.append("Send Rate: " + GetFormatSize(m_Rate/5) + "/sec    ");
    msg.append("Send & Receive Rate: " + GetFormatSize((m_Rate/5)*2) + "/sec");

   // ui->txtBuffer->setPlainText( "Send Rate: " + GetFormatSize(m_Rate) + "/sec" );
   // ui->txtBuffer->appendPlainText( "Send/Receive Rate: " + GetFormatSize(m_Rate*2) + "/sec");

    ui->statusbar->showMessage(msg);


    m_Rate = 0;
}


qint64 MainWindow::SendUDP(const QByteArray &Buff, bool bypasWithDNS){

    QByteArray packet;
    if(bypasWithDNS){
        // packet.append(m_DNSpck.data(), m_DNSSize);
    }

    packet.append(Buff);

    uint sendSize = m_pUDPClient->writeDatagram(packet, QHostAddress("51.195.240.171"), 10001); //khodam
    //if(Buff.length() < 39) return 0;
    // qDebug() <<  "SendUDP() " << Buff.length();

    //return  m_UDPClient->writeDatagram(Buff, QHostAddress("52.43.121.77"), 10001);  //51.75.56.25 8877

     m_Rate += sendSize;       //for get transfer rate monitor

    return sendSize;

    /*
    if(m_UDPClient->writeDatagram(Buff.data(), Buff.size(), QHostAddress::Broadcast, 10001)==-1){
        int test = 1;
    }else{
        m_UDPClient->flush();
    }
    return 1;
    */
}


