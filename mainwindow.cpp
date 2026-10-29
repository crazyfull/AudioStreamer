#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/ravava/AudioDevice.h"
#include "src/ravava/Recorder.h"
#include "src/ravava/Player.h"
#include "src/ravava/AudioProcessor.h"
#include <QDebug>
#include <QUdpSocket>
#include <QTcpSocket>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QFontDatabase>

using namespace ravava;       // the audio library (src/ravava), incl. its network layer (ravava::Packet, ravava::PacketRouter)

MainWindow::MainWindow(QWidget *parent): QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupRouter();

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
    ui->BandWith_comboBox->addItem("Narrowband - [4kHz 8kHz]", OPUS_BANDWIDTH_NARROWBAND);
    ui->BandWith_comboBox->addItem("Medium-band - [6kHz 12kHz]", OPUS_BANDWIDTH_MEDIUMBAND);
    ui->BandWith_comboBox->addItem("Wideband - [8kHz 16kHz]", OPUS_BANDWIDTH_WIDEBAND);
    ui->BandWith_comboBox->addItem("Supper-wideband - [12kHz 24kHz]", OPUS_BANDWIDTH_SUPERWIDEBAND);
    ui->BandWith_comboBox->addItem("fullband - [20kHz 48kHz]", OPUS_BANDWIDTH_FULLBAND);

    refreshAudioDevices();

    ui->noise_comboBox->addItem("Noise: off");
    ui->noise_comboBox->addItem("Noise: low");
    ui->noise_comboBox->addItem("Noise: moderate");
    ui->noise_comboBox->addItem("Noise: high");
    ui->noise_comboBox->addItem("Noise: very high");

    ui->echoLevel_comboBox->addItem("Echo: off");
    ui->echoLevel_comboBox->addItem("Echo: default");
    ui->echoLevel_comboBox->addItem("Echo: protect voice 1");
    ui->echoLevel_comboBox->addItem("Echo: protect voice 2");
    ui->echoLevel_comboBox->addItem("Echo: protect voice 3");

    // built without webrtc-audio-processing: echo / noise / AGC don't exist in this build
    if(!AudioProcessor::IsAvailable()){
        const QString why = "this build has no webrtc-audio-processing";
        ui->echoLevel_comboBox->setCurrentIndex(0);
        ui->noise_comboBox->setCurrentIndex(0);
        ui->autoGain_checkBox->setChecked(false);
        for(QWidget *w : {static_cast<QWidget*>(ui->echoLevel_comboBox), static_cast<QWidget*>(ui->noise_comboBox), static_cast<QWidget*>(ui->autoGain_checkBox)}){
            w->setEnabled(false);
            w->setToolTip(why);
        }
    }

    on_VolumeSlider_valueChanged(ui->VolumeSlider->value());
    on_MicGainSlider_valueChanged(ui->MicGainSlider->value());
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
    m_RxRate = 0;
    ui->statusbar->showMessage("0");

    // ---- live receive statistics (packet loss etc.), below the controls
    m_lblStats = new QLabel(this);
    m_lblStats->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_lblStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lblStats->setContentsMargins(10, 0, 10, 10);
    //ui->gridLayout->addWidget(m_lblStats, 9, 0, 1, 3);
    ui->horizontalLayout_logs->addWidget(m_lblStats);

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

// Lists the sound devices. The first entry is always "System default" (empty id). The selection is
// remembered by device id, so a refresh (new USB headset...) keeps what the user picked.
void MainWindow::refreshAudioDevices()
{
    auto fill = [](QComboBox *box, const std::vector<AudioDeviceInfo> &devices){
        const QSignalBlocker block(box);        // refilling is not a user choice: no device switch
        const QString keep = box->currentData().toString();
        box->clear();
        box->addItem("System default", QString());
        for(const AudioDeviceInfo &d : devices)
            box->addItem(QString::fromUtf8(d.name.c_str()) + (d.isDefault ? " (default)" : ""), QString::fromStdString(d.id));
        int idx = box->findData(keep);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    fill(ui->inputDevice_comboBox, ListInputDevices());
    fill(ui->outputDevice_comboBox, ListOutputDevices());
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

    m_pAudioRecorder = new Recorder();
    m_pAudioPlayer = new Player();
    m_pAudioPlayer->SetVolume(ui->VolumeSlider->value());     // the slider keeps its value across Start/Stop
    m_pAudioRecorder->SetInputGain(ui->MicGainSlider->value());

    // The handler runs on the miniaudio capture thread -> hop to the GUI thread
    // (UDP socket and player are used from there).
    m_pAudioRecorder->SetPacketHandler([this](const Bytes &d){
        QByteArray b(reinterpret_cast<const char*>(d.data()), int(d.size()));
        QMetaObject::invokeMethod(this, [this, b]{ onReadEncodedBuffer(b); }, Qt::QueuedConnection);
    });

    m_AudioConfig.opusBitrate = ui->BitRate_comboBox->currentText().toInt();    //ui->BitRate_comboBox(ui->BitRate_comboBox->currentIndex());
    m_AudioConfig.frameMs  = ui->frameMS_comboBox->currentText().toInt();
    m_AudioConfig.useDtx = ui->useDtx_checkBox->checkState();
    m_AudioConfig.musicMode = ui->musicMode_checkBox->isChecked();
    m_AudioConfig.expectedLossPerc = ui->LossPrec_comboBox->currentText().toInt();
    m_AudioConfig.maxBandwidth = ui->BandWith_comboBox->currentData().toInt();
    m_AudioConfig.playMusic = ui->playMusic_checkBox->checkState();
    m_AudioConfig.musicPath = ui->txtMusicPath->text().toUtf8().toStdString();
    m_AudioConfig.useEchoCancel = ui->echoLevel_comboBox->currentIndex() == 0 ? false: true;
    m_AudioConfig.aecNearendProtection = ui->echoLevel_comboBox->currentIndex();
    m_AudioConfig.noiseSuppression = ui->noise_comboBox->currentIndex();
    m_AudioConfig.autoGain = ui->autoGain_checkBox->isChecked();
    m_AudioConfig.inputDeviceId = ui->inputDevice_comboBox->currentData().toString().toStdString();     // "" = default
    m_AudioConfig.outputDeviceId = ui->outputDevice_comboBox->currentData().toString().toStdString();
    m_AudioConfig.channels = ui->radioButton_mono->isChecked() ? 1: 2;

    // Echo cancellation: ONE instance, the player feeds it what the speaker plays, the recorder
    // cleans the mic with it. (Pointless while playing a music file: the mic is only a clock then.)
    const bool wantAec = m_AudioConfig.useEchoCancel;
    if(AudioProcessor::IsAvailable() && (wantAec || m_AudioConfig.noiseSuppression > 0 || m_AudioConfig.autoGain) && !m_AudioConfig.playMusic){
        auto aec = std::make_shared<AudioProcessor>();
        AudioProcessor::Options opt;
        opt.streamDelayMs = m_AudioConfig.aecDelayMs;
        opt.nearendProtection = m_AudioConfig.aecNearendProtection;
        opt.echoCancel = wantAec;
        opt.noiseSuppression = m_AudioConfig.noiseSuppression;
        opt.autoGain = m_AudioConfig.autoGain;
        std::string err;
        if(aec->Init(m_AudioConfig.inputRate, m_AudioConfig.RenderRate(), m_AudioConfig.channels, opt, &err))
            m_pAec = aec;
        else
            RAVAVA_WARN("echo canceller disabled: %s", err.c_str());
    }
    m_pAudioRecorder->SetAudioProcessor(m_pAec);
    m_pAudioPlayer->SetAudioProcessor(m_pAec);       // before SetConfig: that starts the speaker

    m_pAudioRecorder->SetConfig(&m_AudioConfig);
    m_pAudioPlayer->SetConfig(&m_AudioConfig);

}

// volume of the received audio (0..100), applied by the player on the audio thread
void MainWindow::on_VolumeSlider_valueChanged(int value)
{
    ui->label_volumeValue->setText(QString::number(value));
    if(m_pAudioPlayer)
        m_pAudioPlayer->SetVolume(value);
}

// microphone gain (0..200 %), applied by the recorder on the audio thread
void MainWindow::on_MicGainSlider_valueChanged(int value)
{
    ui->label_micGainValue->setText(QString::number(value));
    if(m_pAudioRecorder)
        m_pAudioRecorder->SetInputGain(value);
}

// The device is switched right away while running; when nothing runs yet, Start picks the choice up (initAudio).
void MainWindow::on_inputDevice_comboBox_currentIndexChanged(int)
{
    m_AudioConfig.inputDeviceId = ui->inputDevice_comboBox->currentData().toString().toStdString();
    if(m_pAudioRecorder && !m_pAudioRecorder->SetInputDevice(m_AudioConfig.inputDeviceId))
        qWarning("microphone: the selected device could not be opened");
}

void MainWindow::on_outputDevice_comboBox_currentIndexChanged(int)
{
    m_AudioConfig.outputDeviceId = ui->outputDevice_comboBox->currentData().toString().toStdString();
    if(m_pAudioPlayer && !m_pAudioPlayer->SetOutputDevice(m_AudioConfig.outputDeviceId))
        qWarning("speaker: the selected device could not be opened");
}

void MainWindow::on_btnStartRecording_clicked()
{
    refreshAudioDevices();      // devices may have been plugged in since the last Start
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

// audio packet body (from the recorder) -> UDP packet (envelope + body) -> network
void MainWindow::onReadEncodedBuffer(const QByteArray &encodedBuffer)
{
    /*
    QDateTime currentTime = QDateTime::currentDateTime();
    qDebug( "Ekhtelaf: [%d]", aTime.time().msecsTo( currentTime.time() ) );
*/


    /**/
    // envelope: packet type + stream id in front of the audio body
    Packet::Header hdr;
    hdr.type = Packet::Audio;
    hdr.streamId = m_txStreamId;
    Bytes wire;
    Packet::WrapInto(wire, hdr, reinterpret_cast<const uint8_t*>(encodedBuffer.constData()), size_t(encodedBuffer.size()));
    const QByteArray datagram(reinterpret_cast<const char*>(wire.data()), int(wire.size()));

    if(ui->LoopBack_checkBox->checkState() == false){
        SendUDP(datagram);              // counts the sent bytes itself (m_Rate): don't count them again here
    }else{
        // network disabled for now: the datagram "arrives" right away, through the same path as a real one
        m_Rate += datagram.size();      // "sent" and "received" at once
        m_RxRate += datagram.size();
        m_router.Dispatch(wire);
    }

    //qDebug() << "onReadEncodedBuffer: " << encodedBuffer.length();
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

        m_RxRate += encodedBuffer.size();       //for get transfer rate monitor

        // the type and the stream id are read here, after the UDP receive: audio goes on to the audio layer,
        // other kinds of packets to their own handlers
        m_router.Dispatch(reinterpret_cast<const uint8_t*>(encodedBuffer.constData()), size_t(encodedBuffer.size()));
    }
}

void MainWindow::setupRouter()
{
    m_router.SetHandler(Packet::Audio, [this](const Packet::Header &hdr, const uint8_t *payload, size_t size){
        onAudioPacket(hdr, payload, size);
    });
    // more packet types: m_router.SetHandler(Packet::Xyz, ...)
}

void MainWindow::onAudioPacket(const Packet::Header &hdr, const uint8_t *payload, size_t size)
{
    // who is talking
    m_rxStreamId = hdr.streamId;
    m_rxStreamTime = QDateTime::currentMSecsSinceEpoch();

    // NOTE: one player = one jitter buffer = one stream. For several people talking at once (conference) each
    // stream id needs its own player; this is where packets get routed to them by hdr.streamId.
    if(m_pAudioPlayer)
        m_pAudioPlayer->Play(payload, size);
}

void MainWindow::OnStatsTimer()
{
    JitterBuffer::Stats st;
    int depthFrames = 0, frameMs = 0;

    if(!m_lblStats)
        return;

    // the devices that are REALLY in use (on Android they can differ from the choice, e.g. the phone's mic although a
    // Bluetooth headset was selected)
    QString devices;
    if(m_pAudioRecorder || m_pAudioPlayer){
        const QString mic = m_pAudioRecorder ? QString::fromStdString(m_pAudioRecorder->ActiveInputDevice()) : QString();
        const QString spk = m_pAudioPlayer ? QString::fromStdString(m_pAudioPlayer->ActiveOutputDevice()) : QString();
        devices = QString("Mic      : %1\nSpeaker  : %2\n").arg(mic.isEmpty() ? "(not running / unknown)" : mic)
                                                        .arg(spk.isEmpty() ? "(not running / unknown)" : spk);
    }

    if(!m_pAudioPlayer || !m_pAudioPlayer->GetStats(st, depthFrames, frameMs)){
        m_lblStats->setText(devices + "Receive stats: (not started, or no audio received yet)");
        return;
    }

    auto pct = [](quint32 lost, quint32 recv){
        quint32 total = recv + lost;
        return total ? 100.0 * lost / total : 0.0;
    };

    // "last 5 seconds": compare with the snapshot taken 5 seconds ago
    // st.lost is a NET number (it goes down again when a packet counted lost arrives late), so its difference
    // is wrong. lostEvents and reordered only ever grow: lost in the window = new losses - new late arrivals.
    // a new stream format (sender changed frame size) starts a new jitter buffer, its counters start at 0 again
    if(!m_statHistory.isEmpty() && st.received < m_statHistory.last().received)
        m_statHistory.clear();
    const StatSnapshot snap = { st.received, st.lostEvents, st.reordered };
    m_statHistory.append(snap);
    while(m_statHistory.size() > 6)
        m_statHistory.removeFirst();
    const auto &old = m_statHistory.first();
    const quint32 recv5 = st.received - old.received;
    const quint32 newLost = st.lostEvents - old.lostEvents;
    const quint32 newLate = st.reordered - old.reordered;
    const quint32 lost5 = newLost > newLate ? newLost - newLate : 0;   // a late one can belong to an earlier window

    QString t = devices;
    t += QString("Received : %1 packets\n").arg(st.received);
    t += QString("Lost     : %1 packets (%2%)   last 5s: %3 (%4%)\n")
             .arg(st.lost).arg(pct(st.lost, st.received), 0, 'f', 1)
             .arg(lost5).arg(pct(lost5, recv5), 0, 'f', 1);
    t += QString("Recovered: FEC %1   PLC %2   too late %3   reordered %4\n")
             .arg(st.fec).arg(st.plc).arg(st.late).arg(st.reordered);
    t += QString("Buffer   : %1 frames (%2 ms)   underruns %3")
             .arg(depthFrames).arg(depthFrames * frameMs).arg(st.underrun);
    if(st.ingestDropped)
        t += QString("   dropped before buffer %1").arg(st.ingestDropped);
    if(st.formatDropped)
        t += QString("   not playable / other format %1").arg(st.formatDropped);

    AudioProcessor::Stats aec;
    if(m_AudioConfig.useEchoCancel && m_pAec && m_pAec->GetStats(aec)){
        t += aec.hasDelay ? QString("\nEcho     : cancelling, estimated echo delay %1 ms").arg(aec.delayMs)
                          : QString("\nEcho     : cancelling (still measuring the delay)");
    }else{
        t += "\nEcho     : off";
    }

    // loudness of the received audio (0..100, loudest since the last refresh) as a text bar
    const int lvl = m_pAudioPlayer->TakePeakLevel();
    t += QString("\nVolume   : [%1%2] %3%")
             .arg(QString(lvl / 5, QChar('#'))).arg(QString(20 - lvl / 5, QChar('-'))).arg(lvl);

    // network layer: who is talking (stream id of the last audio packet) and packets that are not audio
    const PacketRouter::Stats &net = m_router.GetStats();
    if(m_rxStreamTime){
        t += QString("\nStream   : talking stream #%1 (%2 ms ago)")
                 .arg(m_rxStreamId).arg(QDateTime::currentMSecsSinceEpoch() - m_rxStreamTime);
    }
    if(net.unhandled || net.malformed)
        t += QString("\nNetwork  : ignored %1 unknown type, %2 malformed").arg(net.unhandled).arg(net.malformed);
    m_lblStats->setText(t);
}

void MainWindow::OnRateTimer()
{

    QString  msg;
    // measured over the 5 s timer interval; counts the app payload (Opus packet + header), without IP/UDP headers
    msg.append("Send Rate: " + GetFormatSize(m_Rate/5) + "/sec    ");
    msg.append("Receive Rate: " + GetFormatSize(m_RxRate/5) + "/sec    ");
    msg.append("Send & Receive Rate: " + GetFormatSize((m_Rate + m_RxRate)/5) + "/sec");


    // ui->txtBuffer->setPlainText( "Send Rate: " + GetFormatSize(m_Rate) + "/sec" );
    // ui->txtBuffer->appendPlainText( "Send/Receive Rate: " + GetFormatSize(m_Rate*2) + "/sec");

    ui->statusbar->showMessage(msg);


    m_Rate = 0;
    m_RxRate = 0;
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
    qint64 sendSize = 0;        // writeDatagram() returns -1 on error: must stay signed, or the rate counter explodes
    m_packet.append(Buff);
    if(m_packet.length() >= 0){

        //52.20.16.20:40000
        sendSize = m_pUDPClient->writeDatagram(m_packet, QHostAddress("52.43.121.77"), 10001); //public
        //sendSize = m_pUDPClient->writeDatagram(m_packet, QHostAddress("217.154.196.225"), 10001); //kharej khodam
        //sendSize = m_pUDPClient->writeDatagram(m_packet, QHostAddress("95.38.155.49"), 10001); //iran khodam
        //if(Buff.length() < 39) return 0;
        //


        if(sendSize > 0)
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
