#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/clsAudioRecorder.h"

MainWindow::MainWindow(QWidget *parent): QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_pAudioRecorder = new clsAudioRecorder(this);
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

