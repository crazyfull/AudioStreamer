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
    //m_pAudioRecorder->Start();
    m_pAudioRecorder->PlusFrame(960);
    m_pAudioRecorder->Stream();
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
    m_pAudioRecorder->PlusFrame(ui->spinBox->value() );
}

