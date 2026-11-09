#ifndef OpenIGTLSenderPluginInterface_h
#define OpenIGTLSenderPluginInterface_h

#include <QElapsedTimer>
#include <QObject>

#include "ibisapi.h"
#include "toolplugininterface.h"
#include "view.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRenderer.h"
#include "vtkMultiThreader.h"

#include "igtlStatusMessage.h"
#include "igtlServerSocket.h"
#include "VideoStreaming/igtlH264Encoder.h"
#include "igtlUDPServerSocket.h"
#include "igtlMessageRTPWrapper.h"

#include "igtlioCommand.h"

#include "openigtlsenderwidget.h"

// WARNING: this sender only works with the Unity version of the tablet app (ARSpine_Ipad,
// https://github.com/Bigadin/ARSpine_Ipad). Frames are no longer sent as OpenIGTLink H.264 video
// messages but as JPEG images split into raw UDP datagrams with a custom 20 byte header (see
// SendFrameInChunks), which no generic OpenIGTLink client (Slicer, PLUS, the original MARIN iOS
// app) can decode. The H.264 encoder below is still initialised but no longer used for sending.

//User defined parameters to tweak:
//TODO: all of these parameters should go in a config to be placed in .ibis rather than here
// Size of one half of the transmitted image. The frame actually sent is twice as wide: colour on
// the left, the capture alpha as greyscale on the right. JPEG carries no alpha channel, and a
// chroma key cannot survive subsampling, so the mask travels beside the picture instead.
#define SEND_WIDTH 1280
#define SEND_HEIGHT 720
#define SEND_TOTAL_WIDTH ( SEND_WIDTH * 2 )
#define SEND_ASPECT_RATIO (1.*SEND_WIDTH/SEND_HEIGHT)
#define JPEG_QUALITY 75

// Navigation panels: the axial and sagittal reslice views of PedicleScrewNavigation, sent side by
// side as one image. They are opaque panels to be displayed, not an overlay to be keyed, so unlike
// the augmented reality frame they carry no alpha mask.
#define PANEL_WIDTH 320
#define PANEL_HEIGHT 320
#define PANEL_TOTAL_WIDTH ( PANEL_WIDTH * 2 )
// They are read, not registered against, so they do not need the frame rate of the overlay. CT
// slices also compress far worse than the mostly empty 3D view, hence the deliberate throttle.
#define PANEL_PERIOD_MS 100
// Panels appear when the pointer tip comes within this distance of the reference volume bounding
// box, and disappear past the second one. The gap is deliberate: with a single threshold the panel
// would flicker on and off as soon as the operator holds still near the boundary.
#define PANEL_NEAR_MM 5.0
#define PANEL_FAR_MM 15.0

// 20 byte header plus payload, kept under the 1500 byte MTU so a datagram is never IP fragmented
#define CHUNK_HEADER_SIZE 20
#define CHUNK_PAYLOAD_SIZE 1184
// Which image a datagram belongs to, so both can share one socket and run at their own rate
#define STREAM_AR_OVERLAY 0
#define STREAM_NAV_PANELS 1
#define SEND_VIDEO_ON_STARTUP true
// in kbps
#define TARGET_BIT_RATE 10000
#define IMAGE_SEND_MODE RC_TIMESTAMP_MODE
//#define IMAGE_SEND_MODE RC_BITRATE_MODE
#define CONFIG_FILE "OH264Config.cfg"
#define CLIENT_ADDRESS "192.168.3.174"
#define DEVICE_NAME "IBIS"
#define VIDEO_PORT 18946
#define STATUS_PORT 18949
#define MARIN_CAMERA_NAME_IN_CONFIG "iPad"

using namespace igtl;
using namespace std;

enum MAIN_VIEW_TYPE{ _3D, SAGITTAL, CORONAL, TRANSVERSE };

class QLabel;
class QPushButton;
class QUdpSocket;
class vtkWindowToImageFilter;
class vtkRendererSource;
class vtkImageResize;
class VideoStreamIGTLinkServer;

class OpenIGTLSenderPluginInterface : public ToolPluginInterface{

    Q_OBJECT
    Q_INTERFACES(IbisPlugin)
    Q_PLUGIN_METADATA(IID "Ibis.OpenIGTLSenderPluginInterface" )

public:
    OpenIGTLSenderPluginInterface();
    ~OpenIGTLSenderPluginInterface();
    virtual QString GetPluginName() { return QString("OpenIGTLSender"); }
    virtual bool CanRun();
    virtual QString GetMenuEntryString() { return QString("OpenIGTLink Sender"); }

    virtual QWidget * CreateTab();
    virtual bool WidgetAboutToClose();
    void connectVideo();
    void connectStatus();

    bool set_dimensions( int w, int h );
    void toggleVideo(){
        sending_video = !sending_video;
        widget->updateui();
    }
    void toggleStatus(){
        sending_status = !sending_status;
        widget->updateui();
    }
    void changeBandwidth( int kbps );
    void setOffScreen( int state ){
        isOffScreen = (bool)state;
        widget->updateui();
    }
    void ToggleQuadView( bool b ){
        m_displayingQuadView = b;
        widget->updateui();
    }
    void displayQuadView();
    bool isDisplayingQuadView(){ return m_displayingQuadView; }
    QImage getMainView(MAIN_VIEW_TYPE viewType, IbisAPI * api, bool quadView);
    QImage getMainView(MAIN_VIEW_TYPE viewType, IbisAPI * api);

    bool GetSendingVideo() { return sending_video; }
    bool GetConnectedStatus() { return connected_status; }
    bool GetConnectingStatus() { return connecting_status; }
    bool GetSocketCreatedStatus() { return status_socket_created; }

    bool SetClientAddress( QString s );
    std::string GetClientAddress() { return client_address; }

    /** Name of the scene object the navigation panels react to. Empty means the reference volume,
     *  whose bounding box is usually far larger than the anatomy of interest. */
    void SetProximityObjectName( QString s ) { proximityObjectName = s.trimmed(); }
    QString GetProximityObjectName() { return proximityObjectName; }

    /** Split an encoded frame into datagrams and send them. Header is 20 bytes little endian:
     *  uint32 frameId, uint16 chunkIndex, uint16 chunkCount, uint64 timestampMs,
     *  uint8 streamId, uint8 panelsActive, uint16 panelDistanceMm. frameId counts independently
     *  for each stream. */
    void SendFrameInChunks( const QByteArray & encoded, quint8 streamId );

    /** Grab the two reslice views of PedicleScrewNavigation, if it is navigating, and lay them side
     *  by side in bufferPanels. Returns false when there is nothing to send. */
    bool ComposeNavigationPanels();

    /** Distance from the pointer tip to the bounding box of the reference volume, and the resulting
     *  on or off decision. Updates panelsActive and panelDistanceMm. */
    void UpdateProximity();

    //TODO, clean all of this
    OpenIGTLSenderWidget * widget;

    IbisAPI * api;
    View * view;
    vtkRenderer * renderer;
    vtkRenderWindow * window;

    ushort port_video = VIDEO_PORT;
    ushort port_status = STATUS_PORT;
    int version = 4;
    std::string client_address = CLIENT_ADDRESS;
    int clientID = -1;
    uint connection_wait_time = 100000000;
    igtl::TimeStamp::Pointer Timer;
    igtlUint32 currentMessageID = 0;

    igtl::UDPServerSocket::Pointer videoServerSocket;
    igtl::MessageRTPWrapper::Pointer rtpWrapper;
    igtl::MutexLock::Pointer glock;

    igtl::Socket::Pointer socket;
    igtl::ServerSocket::Pointer statusServerSocket;

    bool connected_video = false;
    bool sending_video = false;
    bool video_socket_created = false;

    bool connecting_status = false;
    bool connected_status = false;
    bool sending_status = false;
    bool status_socket_created = false;

    vtkMultiThreader * Threader;
    int ConnectThreadId;
    int SendThreadId;
    bool sendTerminated = false;

    uchar * buffer1;
    //TODO: add a second buffer
//    uchar * buffer2;
    bool init_done = false;
    bool reading = false;
    vtkSmartPointer<vtkWindowToImageFilter> windowToImageFilter;
    vtkSmartPointer<vtkImageResize> resize;
    vtkImageData * img_dat;
    vtkSmartPointer<vtkRendererSource> rendererSource;

    int window_width = SEND_WIDTH;
    int window_height = SEND_HEIGHT;
    QImage window_image, scaled, croped;

    GenericEncoder * encoder = nullptr;
    H264Encoder* H264StreamEncoder;
    bool video_to_send = false;

    // Navigation panels. Captured in the GUI thread like the overlay, encoded and sent in the send
    // thread. Separate buffer and flag so the two streams never wait on each other.
    vtkSmartPointer<vtkWindowToImageFilter> axialToImageFilter;
    vtkSmartPointer<vtkWindowToImageFilter> sagittalToImageFilter;
    uchar * bufferPanels = nullptr;
    bool panels_to_send  = false;
    bool panels_reading  = false;
    QElapsedTimer panelTimer;
    igtlUint32 panelMessageID = 0;

    // Travel in the three reserved header bytes of every datagram of both streams, so the tablet
    // knows the state even while the panels are not being sent.
    bool panelsActive        = false;
    quint16 panelDistanceMm  = 0xFFFF;
    QString proximityObjectName;

    // connectStatus runs in the send thread, so it must never block: it notes the time of a failed
    // attempt here and returns, and the retry happens on a later pass through the loop.
    QElapsedTimer statusRetryTimer;

    bool trackingOK = false;
    bool status_to_send = false;
    void checkTrackingState( IbisAPI * api );

    //TODO: not yet implemented
    bool isOffScreen = true;

private:
    bool m_displayingQuadView = false;

private slots:
    void OnCommandReceived( igtlioCommand * command );
    void OnUpdate();

signals:
    void Modified();

};

#endif
