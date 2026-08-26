cmake_minimum_required(VERSION 3.16)
project(Pierrot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Widgets Concurrent Multimedia QUIET)
if(Qt6_FOUND)
    set(PIERROT_QT_LIB Qt6::Widgets)
    set(PIERROT_QT_CONCURRENT Qt6::Concurrent)
    set(PIERROT_QT_MULTIMEDIA Qt6::Multimedia)
else()
    find_package(Qt5 5.15 COMPONENTS Widgets Concurrent Multimedia REQUIRED)
    set(PIERROT_QT_LIB Qt5::Widgets)
    set(PIERROT_QT_CONCURRENT Qt5::Concurrent)
    set(PIERROT_QT_MULTIMEDIA Qt5::Multimedia)
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
find_package(FFmpeg REQUIRED)

set(SOURCES
    src/main.cpp
    src/CrashReporter.cpp
    src/MainWindow.cpp
    src/models/Project.cpp
    src/ffmpeg/FFmpegDecoder.cpp
    src/ffmpeg/MediaCache.cpp
    src/export/ProjectExporter.cpp
    src/export/LainkaRenderer.cpp
    src/ofx/OfxPluginManager.cpp
    src/ofx/OfxHost.cpp
    src/ofx/OfxRenderer.cpp
    src/ui/MediaPoolWidget.cpp
    src/ui/TimelineWidget.cpp
    src/ui/TimelineCommands.cpp
    src/ui/TimelineSelection.cpp
    src/ui/TimelinePaint.cpp
    src/ui/TimelineDrag.cpp
    src/ui/PreviewWidget.cpp
    src/ui/TlLog.cpp
    src/ui/PancropWidget.cpp
    src/ui/ExportDialog.cpp
    src/ui/ProjectSettingsDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/Theme.cpp
    src/ui/TransformDialog.cpp
    src/ui/AudioEffectsDialog.cpp
    src/ui/TextEditorDialog.cpp
    src/ui/WelcomeWindow.cpp
    src/ui/GraphEditorWidget.cpp
    src/ui/ClickLogger.cpp
    src/ui/EffectsWidget.cpp
    src/ui/ExpressWidget.cpp
    src/ui/FileBrowserWidget.cpp
    src/ui/MixerWidget.cpp
    src/ui/TitleBar.cpp
    src/ui/MesaWidget.cpp
    src/ui/MesaHitTest.cpp
    src/ui/MesaInputHandler.cpp
    src/ui/MesaOverlayRenderer.cpp
    src/render/MesaRenderer.cpp
    resources.qrc
)

set(HEADERS
    src/MainWindow.h
    src/CrashReporter.h
    src/version.h
    src/models/Project.h
    src/ffmpeg/FFmpegDecoder.h
    src/ffmpeg/MediaCache.h
    src/export/ProjectExporter.h
    src/export/LainkaFx.h
    src/export/LainkaRenderer.h
    src/ofx/OfxPluginManager.h
    src/ofx/OfxHost.h
    src/ofx/OfxRenderer.h
    src/ui/MediaPoolWidget.h
    src/ui/TimelineWidget.h
    src/ui/TimelineCommands.h
    src/ui/PreviewWidget.h
    src/ui/PancropWidget.h
    src/ui/ExportDialog.h
    src/ui/ProjectSettingsDialog.h
    src/ui/SettingsDialog.h
    src/ui/Theme.h
    src/ui/TransformDialog.h
    src/ui/AudioEffectsDialog.h
    src/ui/TextEditorDialog.h
    src/ui/WelcomeWindow.h
    src/ui/GraphEditorWidget.h
    src/ui/ClickLogger.h
    src/ui/EffectsWidget.h
    src/ui/ExpressWidget.h
    src/ui/FileBrowserWidget.h
    src/ui/MixerWidget.h
    src/ui/TitleBar.h
    src/ui/MesaWidget.h
    src/render/MesaRenderer.h
)

add_executable(pierrot ${SOURCES} ${HEADERS})

target_include_directories(pierrot PRIVATE src src/ofx/sdk)

target_link_libraries(pierrot PRIVATE
    ${PIERROT_QT_LIB}
    ${PIERROT_QT_CONCURRENT}
    ${PIERROT_QT_MULTIMEDIA}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
)

# dlopen/dlsym para carregar plugins OFX em runtime
target_link_libraries(pierrot PRIVATE ${CMAKE_DL_LIBS})

if(TARGET FFmpeg::swresample)
    target_link_libraries(pierrot PRIVATE FFmpeg::swresample)
else()
    message(FATAL_ERROR "FFmpeg libswresample não encontrada — necessária para decodificar o áudio do preview.")
endif()

install(TARGETS pierrot RUNTIME DESTINATION bin)
install(FILES packaging/pierrot.desktop DESTINATION share/applications)
install(FILES packaging/pierrot.svg DESTINATION share/icons/hicolor/scalable/apps)
