#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/clsAudioRecorder.h"
#include "src/clsAudioPlayer.h"
#include "src/echoCanceller.h"
#include <QDebug>
#include <QUdpSocket>
#include <QTcpSocket>
#include <QLabel>
#include <QGridLayout>
#include <QFontDatabase>

MainWindow::MainWindow(QWidget *parent): QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    uint32_t a = -1;
    if(a <= 1){
        qDebug() << "sizeof " << a;
    }


    //udp connection object
    m_pUDPClient = new QUdpSocket(this);
    connect(m_pUDPClient, SIGNAL(readyRead()),  this, SLOT(onReceiveUDPData()));

    /*
    m_pUDPClient->waitForBytesWritten(200 );
    m_pUDPClient->waitForConnected(200);
    m_pUDPClient->waitForReadyRead(200);
    */

    //bool ret = m_pUDPClient->bind(QHostAddress::AnyIPv4);
    //bool ret = m_pUDPClient->bind(QHostAddress("ws.mojz.ir") , 10001); //QHostAddress("51.75.56.25"),
    //qDebug() << "UDP bind?: " << ret;


    m_AudioConfig.bitRate = 16;
    m_AudioConfig.channels = 1; //3840 1, 7680 2
    m_AudioConfig.inputRate = 48000;
    m_AudioConfig.outputRate = 48000;   //decoder/speaker rate (8000 12000 16000 24000 48000)


    //
    CreateTimer();


    //ui->BandWith_comboBox->addItem("VOICE", OPUS_SIGNAL_VOICE);
    //ui->BandWith_comboBox->addItem("MUSIC", OPUS_SIGNAL_MUSIC);
    ui->BandWith_comboBox->addItem("Narrowband - 4 kHz	8 kHz", OPUS_BANDWIDTH_NARROWBAND);
    ui->BandWith_comboBox->addItem("Medium-band - 6 kHz	12 kHz", OPUS_BANDWIDTH_MEDIUMBAND);
    ui->BandWith_comboBox->addItem("Wideband - 8 kHz	16 kHz", OPUS_BANDWIDTH_WIDEBAND);
    ui->BandWith_comboBox->addItem("Supper-wideband - 12 kHz	24 kHz", OPUS_BANDWIDTH_SUPERWIDEBAND);
    ui->BandWith_comboBox->addItem("fullband - 20 kHz	48 kHz", OPUS_BANDWIDTH_FULLBAND);

    ui->statusbar->showMessage("idle");
}

MainWindow::~MainWindow()
{
    delete m_pAudioRecorder;    // stops the mic first (audio part is Qt-free now, no QObject parent to delete it)
    m_pAudioRecorder = nullptr;
    delete m_pAudioPlayer;
    m_pAudioPlayer = nullptr;
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

    // ---- live receive statistics (packet loss etc.), below the controls
    m_lblStats = new QLabel(this);
    m_lblStats->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_lblStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lblStats->setContentsMargins(10, 0, 10, 10);
    ui->gridLayout->addWidget(m_lblStats, 9, 0, 1, 3);
    OnStatsTimer();

    connect(&m_StatsTimer, SIGNAL(timeout()), this, SLOT(OnStatsTimer()));
    m_StatsTimer.setInterval(1000);
    m_StatsTimer.start();

    /*
    connect(&m_PlayerTimer, SIGNAL(timeout()), this, SLOT(OnPlayerTimer()));
    m_PlayerTimer.setSingleShot(true);
    m_PlayerTimer.setInterval(100);
*/

}

void MainWindow::initAudio()
{
    if(m_pAudioRecorder){
        delete m_pAudioRecorder;
        m_pAudioRecorder = nullptr;
    }

    if(m_pAudioPlayer){
        delete m_pAudioPlayer;
        m_pAudioPlayer = nullptr;

    }

    m_pAec.reset();     // the old recorder/player are gone, nothing uses it anymore

    m_pAudioRecorder = new clsAudioRecorder();
    m_pAudioPlayer = new clsAudioPlayer();

    // The handler runs on the miniaudio capture thread -> hop to the GUI thread
    // (UDP socket and player are used from there).
    m_pAudioRecorder->SetPacketHandler([this](const Bytes &d){
        QByteArray b(reinterpret_cast<const char*>(d.data()), int(d.size()));
        QMetaObject::invokeMethod(this, [this, b]{ onReadEncodedBuffer(b); }, Qt::QueuedConnection);
    });

    m_AudioConfig.opusBitrate = ui->BitRate_comboBox->currentText().toInt();    //ui->BitRate_comboBox(ui->BitRate_comboBox->currentIndex());
    m_AudioConfig.frameMs  = ui->frameMS_comboBox->currentText().toInt();
    m_AudioConfig.useDtx = ui->useDtx_checkBox->checkState();
    m_AudioConfig.expectedLossPerc = ui->LossPrec_comboBox->currentText().toInt();
    m_AudioConfig.MaxBandwith = ui->BandWith_comboBox->currentData().toInt();
    m_AudioConfig.playMusic = ui->playMusic_checkBox->checkState();
    m_AudioConfig.musicPath = ui->txtMusicPath->text().toUtf8().toStdString();
    m_AudioConfig.useEchoCancel = ui->echoCancel_checkBox->isChecked();

    // Echo cancellation: ONE instance, the player feeds it what the speaker plays, the recorder
    // cleans the mic with it. (Pointless while playing a music file: the mic is only a clock then.)
    if(m_AudioConfig.useEchoCancel && !m_AudioConfig.playMusic){
        auto aec = std::make_shared<EchoCanceller>();
        EchoCanceller::Options opt;
        opt.streamDelayMs = m_AudioConfig.aecDelayMs;
        std::string err;
        if(aec->init(m_AudioConfig.inputRate, m_AudioConfig.renderRate(), m_AudioConfig.channels, opt, &err))
            m_pAec = aec;
        else
            AUDIO_WARN("echo canceller disabled: %s", err.c_str());
    }
    m_pAudioRecorder->SetEchoCanceller(m_pAec);
    m_pAudioPlayer->SetEchoCanceller(m_pAec);       // before SetConfig: that starts the speaker

    m_pAudioRecorder->SetConfig(&m_AudioConfig);
    m_pAudioPlayer->SetConfig(&m_AudioConfig);

}

void MainWindow::on_btnStartRecording_clicked()
{
    initAudio();
    m_statHistory.clear();      // new player = new counters
    m_pAudioRecorder->Start();
    //m_PlayerTimer.start();
}


void MainWindow::on_btnStopRecording_clicked()
{
    if(m_pAudioRecorder)
        m_pAudioRecorder->Stop();
    //m_PlayerTimer.stop();
}

void MainWindow::onReadEncodedBuffer(const QByteArray &encodedBuffer)
{
    /*
    QDateTime currentTime = QDateTime::currentDateTime();
    qDebug( "Ekhtelaf: [%d]", aTime.time().msecsTo( currentTime.time() ) );
*/


    /**/
    if(ui->LoopBack_checkBox->checkState() == false){
        SendUDP(encodedBuffer);
    }else{
        // network disabled for now: loopback straight into the player (1 call = 1 opus packet)
        if(m_pAudioPlayer)
            m_pAudioPlayer->Play(reinterpret_cast<const uint8_t*>(encodedBuffer.constData()), size_t(encodedBuffer.size()));
    }

    //qDebug() << "onReadEncodedBuffer: " << encodedBuffer.length();
    m_Rate += encodedBuffer.size();     //for get transfer rate monitor
}

void MainWindow::onReceiveUDPData(){

    // readyRead is NOT emitted once per datagram: drain the socket, otherwise
    // packets pile up in the OS queue and arrive late in bursts.
    while(m_pUDPClient->hasPendingDatagrams()){
        QByteArray encodedBuffer;
        encodedBuffer.resize(int(m_pUDPClient->pendingDatagramSize()));
        m_pUDPClient->readDatagram(encodedBuffer.data(), encodedBuffer.size());

        if(encodedBuffer.isEmpty())
            continue;

        if(m_pAudioPlayer)
            m_pAudioPlayer->Play(reinterpret_cast<const uint8_t*>(encodedBuffer.constData()), size_t(encodedBuffer.size()));
    }
}

void MainWindow::OnStatsTimer()
{
    JitterBuffer::Stats st;
    int depthFrames = 0, frameMs = 0;

    if(!m_lblStats)
        return;

    if(!m_pAudioPlayer || !m_pAudioPlayer->GetStats(st, depthFrames, frameMs)){
        m_lblStats->setText("Receive stats: (press Start)");
        return;
    }

    auto pct = [](quint32 lost, quint32 recv){
        quint32 total = recv + lost;
        return total ? 100.0 * lost / total : 0.0;
    };

    // "last 5 seconds": compare with the snapshot taken 5 seconds ago
    m_statHistory.append(qMakePair(st.received, st.lost));
    while(m_statHistory.size() > 6)
        m_statHistory.removeFirst();
    const auto &old = m_statHistory.first();
    quint32 recv5 = st.received - old.first;
    quint32 lost5 = st.lost > old.second ? st.lost - old.second : 0;

    QString t;
    t += QString("Received : %1 packets\n").arg(st.received);
    t += QString("Lost     : %1 packets (%2%)   last 5s: %3 (%4%)\n")
             .arg(st.lost).arg(pct(st.lost, st.received), 0, 'f', 1)
             .arg(lost5).arg(pct(lost5, recv5), 0, 'f', 1);
    t += QString("Recovered: FEC %1   PLC %2   too late %3   reordered %4\n")
             .arg(st.fec).arg(st.plc).arg(st.late).arg(st.reordered);
    t += QString("Buffer   : %1 frames (%2 ms)   underruns %3")
             .arg(depthFrames).arg(depthFrames * frameMs).arg(st.underrun);

    EchoCanceller::Stats aec;
    if(m_pAec && m_pAec->getStats(aec)){
        t += aec.hasDelay ? QString("\nEcho     : cancelling, estimated echo delay %1 ms").arg(aec.delayMs)
                          : QString("\nEcho     : cancelling (still measuring the delay)");
    }else{
        t += "\nEcho     : off";
    }
    m_lblStats->setText(t);
}

void MainWindow::OnRateTimer()
{

    QString  msg;
    msg.append("Send Rate: " + GetFormatSize(m_Rate/5) + "/sec    ");
    msg.append("Send & Receive Rate: " + GetFormatSize((m_Rate/5)*2) + "/sec");


    // ui->txtBuffer->setPlainText( "Send Rate: " + GetFormatSize(m_Rate) + "/sec" );
    // ui->txtBuffer->appendPlainText( "Send/Receive Rate: " + GetFormatSize(m_Rate*2) + "/sec");

    ui->statusbar->showMessage(msg);


    m_Rate = 0;
}

void MainWindow::OnPlayerTimer()
{

    QDateTime currentTime = QDateTime::currentDateTime();
    int ekhtelaf = aTime.time().msecsTo( currentTime.time() );


    if(ekhtelaf > 135){
        qDebug() << "lost packet send!!!!!!!!!!!!";
        qDebug( "Ekhtelaf: [%d]", ekhtelaf );
        // m_pAudioPlayer->Play(m_playbuffpck);
    }


}

void MainWindow::initTCP()
{
    m_pTCPClient = new QTcpSocket(this);
    connect(m_pTCPClient, SIGNAL(connected()), this, SLOT(OnTCPConnected()) );
    connect(m_pTCPClient, SIGNAL(disconnected()), this, SLOT(OnTCPClosed()) );
    connect(m_pTCPClient, SIGNAL(readyRead()), this, SLOT(OnTCPReceiveData()) );
    connect(m_pTCPClient, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(OnTCPError(QAbstractSocket::SocketError)) );

    //52.43.121.77 9001  echo server
}

void MainWindow::OnTCPConnected()
{
    /*
    int fd = m_Socket.socketDescriptor();
    int iOption = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&iOption, sizeof(iOption));

    */
}

void MainWindow::OnTCPClosed()
{
    // emit onClosed(this);
}

void MainWindow::OnTCPError(QAbstractSocket::SocketError error)
{
    //  emit onError(error, this);
}

void MainWindow::OnTCPReceiveData()
{
    /*
    QByteArray Buffer;
    QByteArray Packet;

    if(!m_cache.isEmpty()){
        Buffer.append(m_cache);
        m_cache.clear();
    }

    Buffer.append(m_Socket.readAll());
    if(m_isFisrtPck == false){
        m_isFisrtPck = true;
        return;
    }


    //parser
    int i = 0;
    int pSize = 0;
    int parserIndex = 0;
    while ((i = Buffer.indexOf("PCK", i)) != -1) {

        Packet = Buffer.mid(pSize, i - pSize);
        emit onReceiveData(Packet, this);


        //qDebug() << "PartSize: " << i - pSize << "Packet:" << Packet;
        parserIndex += (i - pSize) + 3;
        pSize = i + 3;
        ++i;
    }

    if(parserIndex != Buffer.length()){
        m_cache.append( Buffer.mid(parserIndex) );
    }

*/
    //m_Socket.waitForayRead(1000*10000);
    // emit onReceiveData(m_Socket.readAll(), this);
}


qint64 MainWindow::SendUDP(const QByteArray &Buff, bool bypasWithDNS){

    //m_pAudioPlayer->Play(Buff);
    //return 0;

    //qDebug() <<  "SendUDP() " << Buff.length()<< " Time: " <<  QDateTime::currentMSecsSinceEpoch() - lastRecPacketTime;

    //lastRecPacketTime = QDateTime::currentMSecsSinceEpoch();

    if(bypasWithDNS){
        // packet.append(m_DNSpck.data(), m_DNSSize);
    }
    uint sendSize = 0;
    m_packet.append(Buff);
    if(m_packet.length() >= 0){

        //52.20.16.20:40000
        sendSize = m_pUDPClient->writeDatagram(m_packet, QHostAddress("52.43.121.77"), 10001); //public
        //sendSize = m_pUDPClient->writeDatagram(m_packet, QHostAddress("95.38.155.49"), 10001); //khodam
        //if(Buff.length() < 39) return 0;
        //


        m_Rate += sendSize;       //for get transfer rate monitor

        m_packet.clear();
    }



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
