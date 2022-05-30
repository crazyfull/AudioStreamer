#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include "src/AudioConfig.h"
//#include "src/clsAudioRecorder.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
private:
    class clsAudioRecorder *m_pAudioRecorder;
    class clsAudioPlayer *m_pAudioPlayer;
    AudioConfig m_AudioConfig;
    class QUdpSocket *m_pUDPClient;
    QTimer m_RateTimer;
    qint64    m_Rate;

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnStartRecording_clicked();
    void on_btnStopRecording_clicked();
    void on_spinBox_valueChanged(int arg1);
    void onReadEncodedBuffer(const QByteArray &encodedBuffer);
    void onReceiveUDPData();
    void OnRateTimer();

private:
    Ui::MainWindow *ui;

    qint64 SendUDP(const QByteArray &Buff, bool bypasWithDNS = false);
    void CreateTimer();
    QString GetFormatSize(qint64 Size);
};
#endif // MAINWINDOW_H
