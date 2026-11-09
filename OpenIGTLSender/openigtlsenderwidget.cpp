#include "openigtlsenderwidget.h"
#include "ui_openigtlsenderwidget.h"
#include "openigtlsenderplugininterface.h"

OpenIGTLSenderWidget::OpenIGTLSenderWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OpenIGTLSenderWidget)
{
    ui->setupUi(this);
}

OpenIGTLSenderWidget::~OpenIGTLSenderWidget()
{
    delete ui;
}

void OpenIGTLSenderWidget::SetPluginInterface( OpenIGTLSenderPluginInterface * pi )
{
    m_pluginInterface = pi;
    connect( m_pluginInterface, SIGNAL(Modified()), this, SLOT(UpdateUi()) );
    UpdateUi();
}

void OpenIGTLSenderWidget::updateui(){
    UpdateUi();
}

void OpenIGTLSenderWidget::UpdateUi()
{
    //TODO: This should all be cleaned.
    Q_ASSERT( m_pluginInterface );
    //connected video:
    bool cv = m_pluginInterface->GetSendingVideo();
    QString scv = "Connected video: " + QString::number(cv);
    ui->sending_video->setText( scv );

    //connected commands:
    bool cc = m_pluginInterface->GetConnectedStatus();
    QString scc = "Connected commands: " + QString::number(cc);
    ui->connected_commands->setText( scc );

    //connecting commands:
    bool ccc = m_pluginInterface->GetConnectingStatus();
    QString sccc = "Connecting commands: " + QString::number(ccc);
    ui->connecting_commands->setText( sccc );

    //socket created commands:
    bool bscc = m_pluginInterface->GetSocketCreatedStatus();
    QString sscc = "Socket created commands: " + QString::number(bscc);
    ui->socket_created_commands->setText( sscc );

    //client address: left alone while the user is typing in it. UpdateUi runs on every clock tick,
    //so overwriting the field here would undo each keystroke before the next one is entered.
    if( !ui->clientAddress->hasFocus() )
    {
        std::string add = m_pluginInterface->GetClientAddress();
        ui->clientAddress->setText( QString::fromStdString( add ) );
    }

    ui->quadView->setChecked( m_pluginInterface->isDisplayingQuadView() );
}

void OpenIGTLSenderWidget::on_toggleVideo_clicked(){
    Q_ASSERT( m_pluginInterface );
    m_pluginInterface->toggleVideo();
    UpdateUi();
}

void OpenIGTLSenderWidget::on_toggleStatus_clicked(){
    Q_ASSERT( m_pluginInterface );
    m_pluginInterface->toggleStatus();
    UpdateUi();
}

void OpenIGTLSenderWidget::on_BandwidthSpinBox_valueChanged( int kbps ){
    Q_ASSERT( m_pluginInterface );
    m_pluginInterface->changeBandwidth( kbps );
    UpdateUi();
}

void OpenIGTLSenderWidget::on_offScreen_stateChanged( int state ){
    Q_ASSERT( m_pluginInterface );
    m_pluginInterface->setOffScreen( state );
    UpdateUi();
}

void OpenIGTLSenderWidget::on_quadView_stateChanged( int state ){
    Q_ASSERT( m_pluginInterface );
    std::cout << "State is:" << state << std::endl;;
    m_pluginInterface->ToggleQuadView( (bool)state );
    UpdateUi();
}

void OpenIGTLSenderWidget::on_proximityObjectName_textChanged(QString s) {
    Q_ASSERT(m_pluginInterface);
    m_pluginInterface->SetProximityObjectName( s );
}

void OpenIGTLSenderWidget::on_clientAddress_textChanged(QString s) {
    Q_ASSERT(m_pluginInterface);
    m_pluginInterface->SetClientAddress( s );
}

int OpenIGTLSenderWidget::getBandwidth(){
    return ui->BandwidthSpinBox->value();
}

void OpenIGTLSenderWidget::setBandwidth( int bandwidth ){
    ui->BandwidthSpinBox->setValue( bandwidth );
}
