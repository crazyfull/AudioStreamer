#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
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

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_btnStartRecording_clicked();
    void on_btnStopRecording_clicked();
    void on_btnPlusFrame_clicked();
    void on_spinBox_valueChanged(int arg1);
    void onReadEncodedBuffer(const QByteArray &encodedBuffer);

private:
    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
