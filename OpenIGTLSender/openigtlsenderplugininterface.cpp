#include "openigtlsenderplugininterface.h"
#include "scenemanager.h"
#include "pointerobject.h"
#include "imageobject.h"
#include "abstractpolydataobject.h"
#include "application.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>

#include <igtlioConnector.h>

// Reaching into the navigation plugin to capture its two reslice views. Its accessors already
// return null unless it is navigating, so no extra state is needed here.
#include "pediclescrewnavigationplugininterface.h"

#include <vtkWindowToImageFilter.h>
#include <vtkRendererSource.h>
#include <vtkOpenGLRenderer.h>
#include <vtkImageResize.h>
#include <vtkSmartPointer.h>
#include <vtkPNGWriter.h>

#include <algorithm>
#include <cmath>

#include <QBuffer>
#include <QDateTime>
#include <QPainter>
#include <QtEndian>
#include <QStringList>

/////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////   SEND_THREAD   ////////////////////////////////////////
static void * vtkOpenIGTLSenderSendThread( vtkMultiThreader::ThreadInfo * data )
{
    OpenIGTLSenderPluginInterface * self = (OpenIGTLSenderPluginInterface *)(data->UserData);
    self->set_dimensions( SEND_WIDTH, SEND_HEIGHT);
    igtl::StatusMessage::Pointer statusMsg = igtl::StatusMessage::New();
    std::cout << "[SendThread] Created send thread." << std::endl;
    while( !(self->sendTerminated) ){
        if ( !self->init_done ||
             !self->connected_video ||
             ( !self->sending_video && !self->sending_status ) ||
             ( !self->video_to_send && !self->status_to_send && !self->panels_to_send ) ){
            //simtodo: tune this value
            igtl::Sleep(5);
            continue;
        }
        //update on the tracker status:
        if ( self->status_to_send && self->sending_status ){
            if( !self->connected_status && !self->connecting_status ){
                self->connectStatus();
            }
            if( self->connected_status ){
                statusMsg = igtl::StatusMessage::New();
                statusMsg->SetDeviceName( DEVICE_NAME );
                statusMsg->SetHeaderVersion( IGTL_HEADER_VERSION_2 );
                if ( self->trackingOK ){
                    statusMsg->SetCode(igtl::StatusMessage::STATUS_OK);
                    statusMsg->SetSubCode(128);
                    statusMsg->SetErrorName("OK!");
                }else{
                    statusMsg->SetCode(igtl::StatusMessage::STATUS_DISABLED);
                    statusMsg->SetSubCode(128);
                    statusMsg->SetErrorName("NOT_OK!");
                }
                //TODO: eventually add more information, but for now it's just OK / not_OK
                statusMsg->SetStatusString("OK or not OK?");
                statusMsg->Pack();
                std::cout << "[OpenIGTLSenderPlugin] Sending tracking status message." << std::endl << std::flush;
                int send_result = self->socket->Send( statusMsg->GetPackPointer(), statusMsg->GetPackSize() );
                if( send_result < 1 ){
                    std::cerr << "[OpenIGTLSenderPlugin] Status message couldn't be sent. Will try reconnecting and sending it again." << std::endl << std::flush;
                    self->connected_status = false;
                }else{
                    self->status_to_send = false;
                }
            }
        }
        //update on the video:
        if( self->video_to_send ){
            // The composed frame is plain RGB888, so it can be wrapped without copying. 1280 * 3 is
            // a multiple of four, which is the scanline alignment QImage assumes by default.
            self->reading = true;
            QImage frame( self->buffer1, SEND_TOTAL_WIDTH, SEND_HEIGHT, QImage::Format_RGB888 );
            QByteArray encoded;
            QBuffer encodedBuffer( &encoded );
            encodedBuffer.open( QIODevice::WriteOnly );
            if( !frame.save( &encodedBuffer, "JPEG", JPEG_QUALITY ) )
            {
                // Saving needs the Qt jpeg image format plugin to be deployed next to the
                // executable. Without it save() just returns false and nothing is ever sent.
                std::cerr << "[OpenIGTLSenderPlugin] JPEG encoding failed, is the Qt jpeg imageformat plugin deployed?" << std::endl;
                self->reading = false;
                self->video_to_send = false;
                continue;
            }
            encodedBuffer.close();
            self->reading = false;

            self->SendFrameInChunks( encoded, STREAM_AR_OVERLAY );
            self->video_to_send = false;
        }
        //update on the navigation panels:
        if( self->panels_to_send ){
            self->panels_reading = true;
            QImage panels( self->bufferPanels, PANEL_TOTAL_WIDTH, PANEL_HEIGHT, QImage::Format_RGB888 );
            QByteArray encodedPanels;
            QBuffer panelBuffer( &encodedPanels );
            panelBuffer.open( QIODevice::WriteOnly );
            bool ok = panels.save( &panelBuffer, "JPEG", JPEG_QUALITY );
            panelBuffer.close();
            self->panels_reading = false;
            if( ok ) self->SendFrameInChunks( encodedPanels, STREAM_NAV_PANELS );
            self->panels_to_send = false;
        }
    }
    std::cerr << "[Sender] Send thread done." << std::endl << std::flush;
    return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////   CONNECT_THREAD   //////////////////////////////////////
static void * vtkOpenIGTLSenderConnectThread( vtkMultiThreader::ThreadInfo * data )
{
    //get pointer to self and initialize:
    OpenIGTLSenderPluginInterface * self = (OpenIGTLSenderPluginInterface *)(data->UserData);
    if( self->socket.IsNull() ){
    self->connecting_status = true;
    self->socket = self->statusServerSocket->WaitForConnection(self->connection_wait_time);
        std::cout << "[OpenIGTLSenderPlugin] Couldn't connect in time. (TCP for commands)" << std::endl << std::flush;
        self->connected_status = false;
    }else{
        std::cout << "[OpenIGTLSenderPlugin] Connected for commands" << std::endl << std::flush;
        //TODO: need a timeout?
//        self->socket->SetTimeout(3000);
        self->connected_status = true;
    }
    self->connecting_status = false;
    return nullptr;
}

OpenIGTLSenderPluginInterface::OpenIGTLSenderPluginInterface()
{
    videoServerSocket = igtl::UDPServerSocket::New();
    rtpWrapper = igtl::MessageRTPWrapper::New();
    Threader = vtkMultiThreader::New();
    glock = igtl::MutexLock::New();
    socket = igtl::Socket::New();
    statusServerSocket = igtl::ServerSocket::New();
    buffer1 = new uchar[ SEND_TOTAL_WIDTH * SEND_HEIGHT * 3 ];
    bufferPanels = new uchar[ PANEL_TOTAL_WIDTH * PANEL_HEIGHT * 3 ];
    //TODO: eventually add a second buffer?
//    buffer2 = new uchar[ SEND_WIDTH * SEND_HEIGHT * 4 ];
    std::string configPath = GetIbisAPI()->GetConfigDirectory().toStdString() + CONFIG_FILE;
    H264StreamEncoder = new H264Encoder( const_cast<char*>( configPath.c_str() ) );
    int r = statusServerSocket->CreateServer( port_status );
    if( r < 0 )
    {
        std::cerr << "[OpenIGTLSenderPlugin] Cannot create a server socket." << std::endl;
    }
}

bool OpenIGTLSenderPluginInterface::CanRun()
{
    return true;
}

QWidget * OpenIGTLSenderPluginInterface::CreateTab()
{
    //init variables:
    widget = new OpenIGTLSenderWidget;
    widget->SetPluginInterface( this );
    widget->setBandwidth( TARGET_BIT_RATE );
    api = this->GetIbisAPI();
    view = api->GetMain3DView();
    renderer = view->GetRenderer( 0 );
    window = renderer->GetRenderWindow();
    Timer = igtl::TimeStamp::New();
    //connect to ibis update:
    connect( api , SIGNAL(IbisClockTick()), this, SLOT( OnUpdate() ) );
    connect( api, SIGNAL( NewCommandReceived( igtlioCommand * ) ), this, SLOT( OnCommandReceived( igtlioCommand * ) ) );
    SendThreadId = this->Threader->SpawnThread( (vtkThreadFunctionType)&vtkOpenIGTLSenderSendThread, this );
    connectStatus();
    //create vtk pipeline:
    windowToImageFilter = vtkSmartPointer<vtkWindowToImageFilter>::New();
    windowToImageFilter->SetInput( window );
    windowToImageFilter->SetInputBufferTypeToRGBA();
    windowToImageFilter->ReadFrontBufferOn();
    windowToImageFilter->Modified();
    axialToImageFilter    = vtkSmartPointer<vtkWindowToImageFilter>::New();
    sagittalToImageFilter = vtkSmartPointer<vtkWindowToImageFilter>::New();
    panelTimer.start();
    sending_video = SEND_VIDEO_ON_STARTUP;
    videoServerSocket->SetPortNumber( port_video );
    return widget;
}

bool OpenIGTLSenderPluginInterface::WidgetAboutToClose()
{
    disconnect( static_cast<QObject*>( this->GetIbisAPI() ), SIGNAL( IbisClockTick() ), this, SLOT( OnUpdate() ) );
    sendTerminated = true;
    this->Threader->TerminateThread(SendThreadId);
    this->Threader->TerminateThread(ConnectThreadId);
    return true;
}

#include <vtkCamera.h>
void OpenIGTLSenderPluginInterface::OnUpdate()
{
    widget->updateui();
    if( !sending_video ){
        return;
    }
    else if( !connected_video && sending_video ){
        connectVideo();
        return;
    }else if( connected_video && sending_video ){
        // The send thread has not consumed the previous frame yet, so capturing another one would
        // only overwrite work that was never used. Grabbing and rescaling the window is the most
        // expensive thing this plugin does, and at 30 ticks per second most of it was wasted.
        if( video_to_send ){
            return;
        }
        if( m_displayingQuadView ){
            displayQuadView();
        }else{
            windowToImageFilter->Modified();
            windowToImageFilter->Update();
            img_dat = windowToImageFilter->GetOutput();
            int * dims = img_dat->GetDimensions();
            window_width = dims[0];
            window_height = dims[1];
            if (window_width == 0 || window_height == 0) {
                std::cout << "[OpenIGTLSenderPluginInterface::OnUpdate] No window to send" << std::endl;
                return;
            }
            uchar * pixels = static_cast<uchar*>( img_dat->GetScalarPointer() );
            // Keep RGBA: the alpha channel of the captured buffer is what turns the empty parts of
            // the render into the green key colour through CompositionMode_SourceOver below, which
            // is how the tablet knows what to leave transparent. Treating the image as opaque sends
            // the grey background along with the scene.
            window_image = QImage( pixels,
                                       window_width,
                                       window_height,
                                       QImage::Format_RGBA8888 );
            //Crop depends on aspect ratio of current window (since we're using the window for now).
            //simtodo: Eventually it would be better to use off-screen-rendering instead, that way I could set the renderer to the appropriate size directly.
            // The transmitted framing must not depend on the window, or resizing it would silently
            // invalidate the tablet calibration. A vtkCamera applies its view angle to the window
            // HEIGHT and keeps that angle across resizes, so always taking the full height, and 4:3
            // of it in width, transmits the same solid angle whatever the operator does: resizing
            // then changes only how many pixels describe it. The vertical field of view of the
            // transmitted image is exactly the camera view angle, which is what calibration needs.
            // This replaces a width-or-height branch that made the framing follow the window shape.
            const int keptHeight = window_height;
            const int keptWidth  = (int)( window_height * SEND_ASPECT_RATIO + 0.5 );
            // The window has to be at least 4:3 to hold that region. Anything falling outside was
            // never rendered, and QImage::copy zero fills it, so those columns arrive with alpha 0
            // and the tablet shows nothing there rather than a differently framed image.
            croped = window_image.copy( ( window_width - keptWidth ) / 2, 0, keptWidth, keptHeight );
            scaled = croped.scaled( SEND_WIDTH, SEND_HEIGHT, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );

            // Colour on the left, alpha as greyscale on the right. Written row by row rather than
            // with QPainter so no compositing or premultiplication is applied to either half.
            QImage final( SEND_TOTAL_WIDTH, SEND_HEIGHT, QImage::Format_RGB888 );
            for( int y = 0; y < SEND_HEIGHT; ++y )
            {
                // OpenGL has its origin at the bottom left and QImage at the top left, so the rows
                // coming out of vtkWindowToImageFilter are in the opposite order. Reading the source
                // backwards puts the frame the right way up at no cost.
                const uchar * src = scaled.constScanLine( SEND_HEIGHT - 1 - y );
                uchar * dst       = final.scanLine( y );
                for( int x = 0; x < SEND_WIDTH; ++x )
                {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                    uchar a        = src[x * 4 + 3];
                    int maskOffset = ( SEND_WIDTH + x ) * 3;
                    dst[maskOffset + 0] = a;
                    dst[maskOffset + 1] = a;
                    dst[maskOffset + 2] = a;
                }
            }
            if ( !reading )
            {
                //TODO: add second buffer, or is that unnecessary?
                memcpy( buffer1, final.bits(), SEND_TOTAL_WIDTH * SEND_HEIGHT * 3 );
                video_to_send = true;
            }else
            {
                std::cerr << "[Sender] SKIPPED A FRAME" << std::endl;
            }
            init_done = true;
            checkTrackingState( api );
        }

        // Navigation panels, on their own slower clock. They are only worth capturing when the
        // previous one has been sent, otherwise the work would be thrown away like the overlay's.
        if( !panels_to_send && panelTimer.elapsed() >= PANEL_PERIOD_MS )
        {
            panelTimer.restart();
            UpdateProximity();
            if( panelsActive && ComposeNavigationPanels() ) panels_to_send = true;
        }
    }
}

void OpenIGTLSenderPluginInterface::displayQuadView()
{
    QList<ImageObject*> objects = QList<ImageObject*>();
    api->GetAllImageObjects( objects );
    QImage qimg_col_3d = getMainView( MAIN_VIEW_TYPE::_3D, api, true );
    QImage qimg_col_transverse = getMainView( MAIN_VIEW_TYPE::TRANSVERSE, api, true );
    QImage qimg_col_sagittal = getMainView( MAIN_VIEW_TYPE::SAGITTAL, api, true );
    QImage qimg_col_coronal = getMainView( MAIN_VIEW_TYPE::CORONAL, api, true );
    QImage combinedImage = QImage( QSize( SEND_WIDTH, SEND_HEIGHT ), QImage::Format_RGBA8888_Premultiplied );
    QPainter painter( &combinedImage );
    painter.setCompositionMode( QPainter::CompositionMode_Source );
    painter.fillRect( combinedImage.rect(), Qt::black );
    painter.setBrush( QColor( 0, 0, 0, 255 ) );
    painter.drawRect( 0, 0, SEND_WIDTH, SEND_HEIGHT );

    //images don't necessarily have the correct ratio, therefore they need to be resized/scaled/croped:
    QImage sagittal_scaled;
    QImage coronal_scaled;
    QImage transverse_scaled;
    double window_ratio = 1.* qimg_col_coronal.width() / qimg_col_coronal.height();
    if( window_ratio > SEND_ASPECT_RATIO )
    {
        //TODO: on slower system, using other than SmoothTransform might be better
        transverse_scaled = qimg_col_transverse.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        sagittal_scaled = qimg_col_sagittal.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        coronal_scaled = qimg_col_coronal.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( 0, 0, coronal_scaled );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( SEND_WIDTH/2, 0, sagittal_scaled );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( 0, SEND_HEIGHT/2, transverse_scaled );
    }else
    {
        //TODO: this needs to be done (just copy-pasted at the moment)
        transverse_scaled = qimg_col_transverse.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        sagittal_scaled = qimg_col_sagittal.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        coronal_scaled = qimg_col_coronal.scaled( SEND_WIDTH/2, SEND_HEIGHT/2, Qt::KeepAspectRatio, Qt::SmoothTransformation );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( 0, 0, coronal_scaled );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( SEND_WIDTH/2, 0, sagittal_scaled );
        painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
        painter.drawImage( 0, SEND_HEIGHT/2, transverse_scaled );
    }
    painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
    painter.drawImage( SEND_WIDTH/2, SEND_HEIGHT/2, qimg_col_3d );
    painter.end();
    if ( !reading )
    {
        //TODO: add second buffer
        memcpy( buffer1, combinedImage.bits(), SEND_WIDTH * SEND_HEIGHT * 4 );
        video_to_send = true;
    }else
    {
        std::cerr << "[Sender] SKIPPED A FRAME" << std::endl;
    }
    init_done = true;
    checkTrackingState( api );
}

QImage OpenIGTLSenderPluginInterface::getMainView( MAIN_VIEW_TYPE viewType, IbisAPI * api )
{
    return getMainView(viewType, api, false);
}

QImage OpenIGTLSenderPluginInterface::getMainView( MAIN_VIEW_TYPE viewType, IbisAPI * api, bool quadView )
{
    View * view = nullptr;
    switch(viewType){
    case _3D:
        view = api->GetMain3DView();
        break;
    case SAGITTAL:
        view = api->GetMainSagittalView();
        break;
    case CORONAL:
        view = api->GetMainCoronalView();
        break;
    case TRANSVERSE:
        view = api->GetMainTransverseView();
        break;
    }
    vtkRenderer * renderer = view->GetRenderer( 0 );
    vtkRenderWindow * window = renderer->GetRenderWindow();
    vtkSmartPointer<vtkWindowToImageFilter> windowToImageFilter = vtkSmartPointer<vtkWindowToImageFilter>::New();
    windowToImageFilter->SetInput(window);
    windowToImageFilter->SetInputBufferTypeToRGBA();
    windowToImageFilter->ReadFrontBufferOff();
    windowToImageFilter->Update();
    vtkImageData * img_dat = windowToImageFilter->GetOutput();
    uchar * pixels = (uchar*)(img_dat->GetScalarPointer());
    int * dims = img_dat->GetDimensions();
    window_width = dims[0];
    window_height = dims[1];
    QImage image = QImage( (uchar*)(pixels),
                                           window_width,
                                           window_height,
                                           QImage::Format_RGBA8888);
    if(quadView){
        //TODO: here too, use FastTransform on slower machines?
        image = image.scaled(QSize(SEND_WIDTH*0.5, SEND_HEIGHT*0.5), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
 return image;
}

void OpenIGTLSenderPluginInterface::SendFrameInChunks( const QByteArray & encoded, quint8 streamId )
{
    const int encodedSize = (int)encoded.size();
    const int chunkCount  = ( encodedSize + CHUNK_PAYLOAD_SIZE - 1 ) / CHUNK_PAYLOAD_SIZE;
    if( chunkCount == 0 || chunkCount > 65535 ) return;

    // Each stream numbers its frames on its own, so the receiver can tell a new frame from a late
    // fragment of the previous one without the two streams interfering.
    igtlUint32 & frameId = ( streamId == STREAM_NAV_PANELS ) ? panelMessageID : currentMessageID;

    const quint64 stamp = (quint64)QDateTime::currentMSecsSinceEpoch();
    QByteArray packet( CHUNK_HEADER_SIZE + CHUNK_PAYLOAD_SIZE, Qt::Uninitialized );
    uchar * header = (uchar *)packet.data();

    qToLittleEndian<quint32>( (quint32)frameId, header );
    qToLittleEndian<quint16>( (quint16)chunkCount, header + 6 );
    qToLittleEndian<quint64>( stamp, header + 8 );
    header[16] = streamId;
    // Proximity state rides on every datagram of both streams, so the tablet knows whether the
    // panels are meant to be shown even during the long stretches where none are sent.
    header[17] = panelsActive ? 1 : 0;
    qToLittleEndian<quint16>( panelDistanceMm, header + 18 );

    for( int i = 0; i < chunkCount; ++i )
    {
        const int offset = i * CHUNK_PAYLOAD_SIZE;
        const int length = qMin( CHUNK_PAYLOAD_SIZE, encodedSize - offset );
        qToLittleEndian<quint16>( (quint16)i, header + 4 );
        memcpy( header + CHUNK_HEADER_SIZE, encoded.constData() + offset, length );
        videoServerSocket->WriteSocket( header, CHUNK_HEADER_SIZE + length );
    }
    frameId++;
}

void OpenIGTLSenderPluginInterface::UpdateProximity()
{
    panelDistanceMm = 0xFFFF;

    PointerObject * navPointer = api->GetNavigationPointerObject();
    if( !navPointer )
    {
        panelsActive = false;
        return;
    }

    // A named object if one was given, otherwise the reference volume. The bounding box of a CT
    // covers far more than the anatomy it contains, so naming the surface of interest instead gives
    // a threshold that means something.
    SceneObject * target = nullptr;
    double bounds[6];
    bool haveBounds = false;

    if( !proximityObjectName.isEmpty() )
    {
        QList<SceneObject *> all;
        api->GetAllUserObjects( all );
        foreach( SceneObject * obj, all )
        {
            if( QString::compare( obj->GetName(), proximityObjectName, Qt::CaseInsensitive ) == 0 )
            {
                target = obj;
                break;
            }
        }
    }
    if( !target ) target = api->GetReferenceDataObject();
    if( !target )
    {
        panelsActive = false;
        return;
    }

    // Surfaces and volumes both carry bounds, they just live behind different accessors.
    AbstractPolyDataObject * asSurface = AbstractPolyDataObject::SafeDownCast( target );
    ImageObject * asVolume             = ImageObject::SafeDownCast( target );
    if( asSurface && asSurface->GetPolyData() )
    {
        asSurface->GetPolyData()->GetBounds( bounds );
        haveBounds = true;
    }
    else if( asVolume && asVolume->GetImage() )
    {
        asVolume->GetImage()->GetBounds( bounds );
        haveBounds = true;
    }
    if( !haveBounds )
    {
        panelsActive = false;
        return;
    }

    // The bounding box is expressed in the object own coordinates, so the tip is brought into that
    // space rather than the box into the world. Registration transforms here are rigid, so the
    // distance is the same measured on either side.
    vtkSmartPointer<vtkTransform> inverseImageTransform = vtkSmartPointer<vtkTransform>::New();
    inverseImageTransform->SetMatrix( target->GetWorldTransform()->GetMatrix() );
    inverseImageTransform->Inverse();
    double * tipWorld = navPointer->GetTipPosition();
    double * tip      = inverseImageTransform->TransformPoint( tipWorld );

    // Distance from a point to an axis aligned box: zero on each axis where the point is already
    // within the slab, so the result is zero everywhere inside the box.
    double offset[3];
    for( int i = 0; i < 3; ++i )
    {
        double belowMin = bounds[2 * i] - tip[i];
        double aboveMax = tip[i] - bounds[2 * i + 1];
        offset[i]       = std::max( 0.0, std::max( belowMin, aboveMax ) );
    }
    double distance = std::sqrt( offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] );

    panelDistanceMm = (quint16)std::min( distance, 65534.0 );

    // Hysteresis: the two thresholds differ so holding still near the limit does not toggle.
    if( !panelsActive && distance <= PANEL_NEAR_MM ) panelsActive = true;
    else if( panelsActive && distance > PANEL_FAR_MM ) panelsActive = false;
}

bool OpenIGTLSenderPluginInterface::ComposeNavigationPanels()
{
    // The accessors return null unless the plugin is actually navigating, which is exactly the
    // condition under which these panels are worth sending. No extra state to track.
    ToolPluginInterface * plugin = api->GetToolPluginByName( "PedicleScrewNavigation" );
    PedicleScrewNavigationPluginInterface * nav =
        dynamic_cast<PedicleScrewNavigationPluginInterface *>( plugin );
    if( !nav ) return false;

    vtkRenderer * axialRenderer    = nav->GetScrewNavigationAxialRenderer();
    vtkRenderer * sagittalRenderer = nav->GetScrewNavigationSagittalRenderer();
    if( !axialRenderer || !sagittalRenderer ) return false;

    vtkRenderWindow * axialWindow    = axialRenderer->GetRenderWindow();
    vtkRenderWindow * sagittalWindow = sagittalRenderer->GetRenderWindow();
    if( !axialWindow || !sagittalWindow ) return false;

    // RGB rather than RGBA: these panels are displayed as they are, never keyed, so the alpha
    // channel would be read back for nothing.
    if( axialToImageFilter->GetInput() != axialWindow )
    {
        axialToImageFilter->SetInput( axialWindow );
        axialToImageFilter->SetInputBufferTypeToRGB();
        axialToImageFilter->ReadFrontBufferOn();
    }
    if( sagittalToImageFilter->GetInput() != sagittalWindow )
    {
        sagittalToImageFilter->SetInput( sagittalWindow );
        sagittalToImageFilter->SetInputBufferTypeToRGB();
        sagittalToImageFilter->ReadFrontBufferOn();
    }

    QImage panels( PANEL_TOTAL_WIDTH, PANEL_HEIGHT, QImage::Format_RGB888 );
    panels.fill( Qt::black );

    vtkWindowToImageFilter * filters[2] = { axialToImageFilter, sagittalToImageFilter };
    for( int side = 0; side < 2; ++side )
    {
        filters[side]->Modified();
        filters[side]->Update();
        vtkImageData * grabbed = filters[side]->GetOutput();
        int dims[3];
        grabbed->GetDimensions( dims );
        if( dims[0] == 0 || dims[1] == 0 ) return false;

        // The explicit bytes per line matters: vtkWindowToImageFilter packs rows tightly while
        // QImage assumes a four byte alignment, and width * 3 is not always a multiple of four.
        QImage grabbedImage( static_cast<uchar *>( grabbed->GetScalarPointer() ), dims[0], dims[1],
                             dims[0] * 3, QImage::Format_RGB888 );
        QImage panelImage =
            grabbedImage.scaled( PANEL_WIDTH, PANEL_HEIGHT, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );

        // Same origin mismatch as the overlay: OpenGL counts rows from the bottom, QImage from the
        // top, so the source is read backwards.
        const int xOffset = side * PANEL_WIDTH;
        for( int y = 0; y < PANEL_HEIGHT; ++y )
        {
            const uchar * src = panelImage.constScanLine( PANEL_HEIGHT - 1 - y );
            uchar * dst       = panels.scanLine( y ) + xOffset * 3;
            memcpy( dst, src, PANEL_WIDTH * 3 );
        }
    }

    if( panels_reading ) return false;
    memcpy( bufferPanels, panels.bits(), PANEL_TOTAL_WIDTH * PANEL_HEIGHT * 3 );
    return true;
}

void OpenIGTLSenderPluginInterface::changeBandwidth( int kbps )
{
    int netWorkBandWidthInBPS = kbps * 1000; //networkBandwidth is in kbps
    int time = floor( 8*RTP_PAYLOAD_LENGTH*1e9/netWorkBandWidthInBPS + 1.0 ); // the needed time in nanosecond to send a RTP payload.
    rtpWrapper->packetIntervalTime = time;
    //set encoder target too:
    H264StreamEncoder->SetRCTargetBitRate( netWorkBandWidthInBPS );
    std::cout << "[OpenIGTLSenderPluginInterface::changeBandwidth] Set RTPWrapper bandwidth to " << kbps << " kbps." << std::endl;
}


OpenIGTLSenderPluginInterface::~OpenIGTLSenderPluginInterface()
{
    //TODO
}

void OpenIGTLSenderPluginInterface::connectVideo() {
    if( !video_socket_created ){
        connected_video = false;
        int s = videoServerSocket->CreateUDPServer();
        if( s < 0 ){
            std::cerr << "[OpenIGTLSenderPlugin] Could not create a UDP server socket for video." << std::endl;
        }else{
            video_socket_created = true;
        }
    }
    if( video_socket_created && !connected_video ){
        clientID = videoServerSocket->AddClient( client_address.c_str(), port_video, 0 );
        std::cout << "[Sender] added client: " << clientID << std::endl;
        std::cout << "[OpenIGTLSenderPlugin] Created a UDP server socket for video." << std::endl;
        connected_video = true;
    }
    widget->updateui();
}

void OpenIGTLSenderPluginInterface::connectStatus()
{
    if( !status_socket_created ){
        // This is called from the send thread. Sleeping here used to stall the video for the whole
        // delay, so the stream advanced one frame every five seconds whenever the status port was
        // busy. The retry is now spaced out without blocking anything.
        if( statusRetryTimer.isValid() && statusRetryTimer.elapsed() < 5000 ) return;
        int s = statusServerSocket->CreateServer( port_status );
        if( s < 0 ){
            std::cerr << "[OpenIGTLSenderPlugin] Could not create a TCP server socket for status updates. This typically means that the socket from a previously running instance of IBIS hasn't yet been closed. I will retry in a few seconds." << std::endl;
            statusRetryTimer.restart();
            return;
        }else{
            status_socket_created = true;
        }
    }
    ConnectThreadId = Threader->SpawnThread( (vtkThreadFunctionType)&(vtkOpenIGTLSenderConnectThread), this );
    widget->updateui();
}

bool OpenIGTLSenderPluginInterface::set_dimensions( int w, int h )
{
    std::cout << "[OpenIGTLSenderPluginInterface] Dimensions of the H264 encoder have been set to:" << w << " x " << h << std::endl;
    H264StreamEncoder->SetPicWidthAndHeight( w, h );
    //simtodo: could thinker with these values and see what works best
    H264StreamEncoder->SetSpeed( HIGH_COMPLEXITY );
    H264StreamEncoder->InitializeEncoder();
    H264StreamEncoder->SetRCMode( IMAGE_SEND_MODE );
    changeBandwidth( TARGET_BIT_RATE );
    encoder = H264StreamEncoder;
    return true;
}

//TODO: this whole function should be cleaned
void OpenIGTLSenderPluginInterface::checkTrackingState( IbisAPI * api)
{
    QString supposed_name_of_device = QString( MARIN_CAMERA_NAME_IN_CONFIG );
    QList<TrackedSceneObject*> all;
    api->GetAllTrackedObjects( all );
    bool allOK = false;
    bool iPadExists = false;
    foreach(TrackedSceneObject* obj, all){
        QString objname = obj->GetName();
        if ( QString::compare( objname, supposed_name_of_device, Qt::CaseInsensitive) == 0 ) {
            if ( obj->GetState() != TrackerToolState::Ok ) { allOK = false; }
            else{ allOK = true; }
            iPadExists = true;
        }
    }
    //if something has changed:
    if ( allOK != trackingOK ){
        status_to_send = true;
        if(allOK)
            std::cout << "[OpenIGTLSenderPlugin][DEBUG]: Camera tracking is back." << std::endl;
        else
            std::cout << "[OpenIGTLSenderPlugin][DEBUG]: Camera tracking lost." << std::endl;
    }
    trackingOK = allOK;
}

bool OpenIGTLSenderPluginInterface::SetClientAddress( QString s )
{
    // Only act on a complete address. This used to run on every keystroke, and since
    // UDPServerSocket::DeleteClient walks the group list instead of the client list it never removes
    // anything: every half typed address stayed in the client list for good. WriteSocket sends each
    // packet to every client, so one unreachable entry costs an ARP timeout of about three seconds
    // per packet, which is what stalled the video.
    // Checked by hand rather than with QHostAddress, which lives in QtNetwork and is an optional
    // module here ( IBIS_USE_QT_NETWORK is off by default ).
    QStringList parts = s.split( '.' );
    if( parts.size() != 4 ) return false;
    for( const QString & part : parts )
    {
        bool ok = false;
        int value = part.toInt( &ok );
        if( !ok || part.isEmpty() || value < 0 || value > 255 ) return false;
    }
    if( s.toStdString() == client_address ) return true;

    std::cout << "[OpenIGTLSenderPlugin][SetClientAddress]: Changing client address to: " << s.toStdString() << std::endl;
    client_address = s.toStdString();

    // The client list cannot be pruned, so the socket is rebuilt rather than added to.
    videoServerSocket->CloseSocket();
    videoServerSocket = igtl::UDPServerSocket::New();
    videoServerSocket->SetPortNumber( port_video );
    video_socket_created = false;
    connected_video = false;
    return true;
}

void OpenIGTLSenderPluginInterface::OnCommandReceived( igtlioCommand * command ){
    if( command->GetName() == "ToggleQuadView" ){
        bool toggled = false;
        std::string commandContent = command->GetCommandContent();
        QDomDocument qdd  = QDomDocument();
        QByteArray qba = QByteArray( commandContent.c_str(), commandContent.length() );
        if( commandContent.length() > 0 ){
            QDomDocument::ParseResult parseResult = qdd.setContent( qba );
            //this case would happen if the xml string isn't formated properly
            if( !parseResult ){ std::cout << "[OpenIGTLSenderPluginInterface::OnCommandReceived] couldn't read command message parameters." << std::endl; }
            //check for known params:
            QDomNodeList qdnl;
            qdnl = qdd.elementsByTagName(QString("par4"));
            toggled = static_cast<bool>( qdnl.at(0).toElement().text().toInt() );
        }else{
            std::cerr << "[OpenIGTLSenderPluginInterface::OnCommandReceived] Received a 0 length command." << std::endl;
        }
        ToggleQuadView( toggled );
        std::cout << "[OpenIGTLSenderPluginInterface::OnCommandReceived] Toggled quadView to " << toggled << std::endl;
    }
}
