#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QTcpSocket>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <memory>

#include "src/AudioConfig.h"
//#include "src/clsAudioRecorder.h"

class AudioProcessor;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
private:
    class clsAudioRecorder *m_pAudioRecorder = nullptr;
    class clsAudioPlayer *m_pAudioPlayer  = nullptr;
    std::shared_ptr<AudioProcessor> m_pAec;      // shared by recorder (mic) and player (speaker)
    AudioConfig m_AudioConfig;
    class QUdpSocket *m_pUDPClient;
    class QTcpSocket *m_pTCPClient;

    QTimer m_RateTimer;
    QTimer m_PlayerTimer;
    QTimer m_StatsTimer;                  // refreshes the live loss statistics (1s)
    class QLabel *m_lblStats = nullptr;
    struct StatSnapshot { quint32 received, lostEvents, reordered; };
    QList<StatSnapshot> m_statHistory;      // once per second, for the "last 5s" figures

    qint64    m_Rate;
    QByteArray m_lostpck;
    QDateTime aTime;
    QDateTime bTime;
    QByteArray m_playbuffpck;
    qint64 lastRecPacketTime;
    QByteArray m_packet;



public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnStartRecording_clicked();
    void on_btnStopRecording_clicked();
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
    qint64 SendUDP(const QByteArray &Buff, bool bypasWithDNS = false);
    void CreateTimer();
    QString GetFormatSize(qint64 Size);
    void initAudio();

};


#endif // MAINWINDOW_H
