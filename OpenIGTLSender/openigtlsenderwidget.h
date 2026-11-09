#ifndef OpenIGTLSenderWidget_h
#define OpenIGTLSenderWidget_h

#include <QWidget>

class OpenIGTLSenderPluginInterface;

namespace Ui
{
    class OpenIGTLSenderWidget;
}


class OpenIGTLSenderWidget : public QWidget
{

    Q_OBJECT

public:

    explicit OpenIGTLSenderWidget( QWidget *parent = nullptr );
    ~OpenIGTLSenderWidget();

    void SetPluginInterface( OpenIGTLSenderPluginInterface * pi );
    int getBandwidth();
    void setBandwidth( int bandwidth );
    void updateui();

private slots:

    void UpdateUi();
    void on_toggleVideo_clicked();
    void on_toggleStatus_clicked();
    void on_BandwidthSpinBox_valueChanged( int kbps );
    void on_offScreen_stateChanged( int state );
    void on_quadView_stateChanged( int state );
    void on_clientAddress_textChanged( QString s );
    void on_proximityObjectName_textChanged( QString s );

private:

    OpenIGTLSenderPluginInterface * m_pluginInterface;
    Ui::OpenIGTLSenderWidget * ui;

};

#endif
