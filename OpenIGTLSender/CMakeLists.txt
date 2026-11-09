find_package( OpenIGTLink REQUIRED )
find_package( OpenIGTLinkIO REQUIRED )

set( PluginSrc openigtlsenderplugininterface.cpp openigtlsenderwidget.cpp )
set( PluginHdrMoc openigtlsenderwidget.h openigtlsenderplugininterface.h )
set( PluginUi openigtlsenderwidget.ui )

DefinePlugin( "${PluginSrc}" "${PluginHdr}" "${PluginHdrMoc}" "${PluginUi}" )
target_link_libraries( ${PluginName} ${OpenIGTLinkIO_LIBRARIES} ${OpenIGTLink_LIBRARIES} )

# The sender captures the axial and sagittal reslice views of the navigation plugin. Each plugin
# exports its own source directory as PUBLIC, so linking the target also brings in its header.
# PedicleScrewNavigation therefore has to be enabled in the build.
target_link_libraries( ${PluginName} PedicleScrewNavigation )

find_library( YUV_LIB libyuv.a "/optional/path/to/libyuv/build" )
find_path( YUV_LIB_HEADERS libyuv.h /optional/path/libyuv/include )

include_directories( ${YUV_LIB_HEADERS} )

target_link_libraries( ${PluginName} ${YUV_LIB} )
