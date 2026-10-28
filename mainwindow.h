#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QTcpSocket>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <memory>

#include "src/ravava/AudioConfig.h"
#include "src/ravava/net/PacketRouter.h"
//#include "src/ravava/Recorder.h"

namespace ravava { class AudioProcessor; class Recorder; class Player; }

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
private:
    ravava::Recorder *m_pAudioRecorder = nullptr;
    ravava::Player *m_pAudioPlayer  = nullptr;
    std::shared_ptr<ravava::AudioProcessor> m_pAec;      // shared by recorder (mic) and player (speaker)
    ravava::AudioConfig m_AudioConfig;
    class QUdpSocket *m_pUDPClient;
    class QTcpSocket *m_pTCPClient;

    QTimer m_RateTimer;
    QTimer m_PlayerTimer;
    QTimer m_StatsTimer;                  // refreshes the live loss statistics (1s)
    class QLabel *m_lblStats = nullptr;
    struct StatSnapshot { quint32 received, lostEvents, reordered; };
    QList<StatSnapshot> m_statHistory;      // once per second, for the "last 5s" figures

    qint64    m_Rate;       // bytes sent in the current 5 s window
    qint64    m_RxRate;     // bytes received in the current 5 s window
    QByteArray m_lostpck;
    QDateTime aTime;
    QDateTime bTime;
    QByteArray m_playbuffpck;
    qint64 lastRecPacketTime;
    QByteArray m_packet;

    // Network layer: the UDP envelope (packet type + stream id, src/ravava/net/Packet.h) is handled HERE.
    // The audio layer only ever gets the payload of Packet::Audio packets.
    ravava::PacketRouter m_router;
    quint16 m_txStreamId = 0;       // stream id written into the packets we send
    quint16 m_rxStreamId = 0;       // stream id of the last audio packet received = who is talking
    qint64  m_rxStreamTime = 0;     // when it arrived (ms since epoch), 0 = nothing yet



public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnStartRecording_clicked();
    void on_btnStopRecording_clicked();
    void on_VolumeSlider_valueChanged(int value);
    void onReadEncodedBuffer(const QByteArray &encodedBuffer);
    void onReceiveUDPData();
    void OnRateTimer();
    void OnStatsTimer();
    void OnPlayerTimer();

    void OnTCPConnected();
    void OnTCPClosed();
    void OnTCPReceiveData();
    void OnTCPError(QAbstractSocket::SocketError error);

private:
    Ui::MainWindow *ui;
    void initTCP();
    void setupRouter();
    void onAudioPacket(const ravava::Packet::Header &hdr, const uint8_t *payload, size_t size);
    qint64 SendUDP(const QByteArray &Buff, bool bypasWithDNS = false);
    void CreateTimer();
    QString GetFormatSize(qint64 Size);
    void initAudio();

};


#endif // MAINWINDOW_H
