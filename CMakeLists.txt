cmake_minimum_required(VERSION 3.16)
project(Pierrot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Widgets Concurrent Multimedia Svg Test QUIET)
if(Qt6_FOUND)
    set(PIERROT_QT_LIB Qt6::Widgets)
    set(PIERROT_QT_CONCURRENT Qt6::Concurrent)
    set(PIERROT_QT_MULTIMEDIA Qt6::Multimedia)
    set(PIERROT_QT_CORE Qt6::Core)
    set(PIERROT_QT_GUI Qt6::Gui)
    set(PIERROT_QT_SVG Qt6::Svg)
    set(PIERROT_QT_TEST Qt6::Test)
else()
    find_package(Qt5 5.15 COMPONENTS Widgets Concurrent Multimedia Svg Test REQUIRED)
    set(PIERROT_QT_LIB Qt5::Widgets)
    set(PIERROT_QT_CONCURRENT Qt5::Concurrent)
    set(PIERROT_QT_MULTIMEDIA Qt5::Multimedia)
    set(PIERROT_QT_CORE Qt5::Core)
    set(PIERROT_QT_GUI Qt5::Gui)
    set(PIERROT_QT_SVG Qt5::Svg)
    set(PIERROT_QT_TEST Qt5::Test)
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
find_package(FFmpeg REQUIRED)

# ── Colombina: o kernel, a camada blindada (quase não muda) ─────────────
# models/, ffmpeg/, render/, export/, ofx/. Não pode incluir src/ui — a
# barreira é estrutural: nada aqui pode depender da casca (widgets).
set(COLOMBINA_SOURCES
    src/colombina/models/Project.cpp
    src/colombina/ffmpeg/FFmpegDecoder.cpp
    src/colombina/ffmpeg/AudioConformCache.cpp
    src/colombina/ffmpeg/MediaCache.cpp
    src/colombina/ffmpeg/ProxyManager.cpp
    src/colombina/export/ProjectExporter.cpp
    src/colombina/export/LainkaRenderer.cpp
    src/colombina/export/OfxExportRenderer.cpp
    src/colombina/export/NleInterchange.cpp
    src/colombina/ofx/OfxPluginManager.cpp
    src/colombina/ofx/OfxHost.cpp
    src/colombina/ofx/OfxRenderer.cpp
    src/colombina/render/MesaRenderer.cpp
)

set(COLOMBINA_HEADERS
    src/colombina/models/Project.h
    src/colombina/ffmpeg/FFmpegDecoder.h
    src/colombina/ffmpeg/MediaCache.h
    src/colombina/ffmpeg/ProxyManager.h
    src/colombina/export/ProjectExporter.h
    src/colombina/export/LainkaFx.h
    src/colombina/export/LainkaRenderer.h
    src/colombina/export/OfxExportRenderer.h
    src/colombina/export/NleInterchange.h
    src/colombina/ofx/OfxPluginManager.h
    src/colombina/ofx/OfxHost.h
    src/colombina/ofx/OfxRenderer.h
    src/colombina/render/MesaRenderer.h
    src/colombina/generators.h
    src/colombina/util.h
)

# ── Casca: a UI (muda o tempo todo) ────────────────────────────────────
set(PIERROT_APP_SOURCES
    src/main.cpp
    src/CrashReporter.cpp
    src/Bench.cpp
    src/MainWindow.cpp
    src/ui/MediaPoolWidget.cpp
    src/ui/TimelineWidget.cpp
    src/ui/TimelineCommands.cpp
    src/ui/TimelineSelection.cpp
    src/ui/TimelinePaint.cpp
    src/ui/TimelineDrag.cpp
    src/ui/PlaybackEngine.cpp
    src/ui/PreviewWidget.cpp
    src/ui/TlLog.cpp
    src/ui/PancropWidget.cpp
    src/ui/ExportDialog.cpp
    src/ui/ProjectSettingsDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/Theme.cpp
    src/ui/TransformDialog.cpp
    src/ui/MaskEditorDialog.cpp
    src/ui/AudioEffectsDialog.cpp
    src/ui/TrackAudioFxDialog.cpp
    src/ui/TextEditorDialog.cpp
    src/ui/WelcomeWindow.cpp
    src/ui/GraphEditorWidget.cpp
    src/ui/ClipPropertiesWidget.cpp
    src/ui/ClickLogger.cpp
    src/ui/EffectsWidget.cpp
    src/ui/ExpressWidget.cpp
    src/ui/TrimmerDialog.cpp
    src/ui/RenderQueueDialog.cpp
    src/ui/ScopeWidget.cpp
    src/ui/PreviewMonitor.cpp
    src/ui/FileBrowserWidget.cpp
    src/ui/MixerWidget.cpp
    src/ui/TitleBar.cpp
    src/ui/MesaWidget.cpp
    src/ui/MesaHitTest.cpp
    src/ui/MesaInputHandler.cpp
    src/ui/MesaOverlayRenderer.cpp
    src/ui/MesaCanvasOps.cpp
    resources.qrc
)

set(PIERROT_APP_HEADERS
    src/MainWindow.h
    src/CrashReporter.h
    src/Bench.h
    src/clipattrs.h
    src/version.h
    src/ui/TrimmerDialog.h
    src/ui/RenderQueueDialog.h
    src/ui/ScopeWidget.h
    src/ui/MediaPoolWidget.h
    src/ui/TimelineWidget.h
    src/ui/TimelineCommands.h
    src/ui/PreviewWidget.h
    src/ui/PlaybackEngine.h
    src/ui/PreviewMonitor.h
    src/ui/PancropWidget.h
    src/ui/ExportDialog.h
    src/ui/ProjectSettingsDialog.h
    src/ui/SettingsDialog.h
    src/ui/Theme.h
    src/ui/TransformDialog.h
    src/ui/MaskEditorDialog.h
    src/ui/AudioEffectsDialog.h
    src/ui/TrackAudioFxDialog.h
    src/ui/TextEditorDialog.h
    src/ui/WelcomeWindow.h
    src/ui/GraphEditorWidget.h
    src/ui/ClipPropertiesWidget.h
    src/ui/ClickLogger.h
    src/ui/EffectsWidget.h
    src/ui/ExpressWidget.h
    src/ui/FileBrowserWidget.h
    src/ui/MixerWidget.h
    src/ui/TitleBar.h
    src/ui/MesaWidget.h
)

# ── Alvos ───────────────────────────────────────────────────────────────
add_library(colombina STATIC ${COLOMBINA_SOURCES} ${COLOMBINA_HEADERS})

target_include_directories(colombina PUBLIC src src/colombina/ofx/sdk)

target_link_libraries(colombina PUBLIC
    ${PIERROT_QT_CONCURRENT}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
)

# dlopen/dlsym para carregar plugins OFX em runtime
target_link_libraries(colombina PUBLIC ${CMAKE_DL_LIBS})

if(TARGET FFmpeg::swresample)
    target_link_libraries(colombina PUBLIC FFmpeg::swresample)
else()
    message(FATAL_ERROR "FFmpeg libswresample não encontrada — necessária para decodificar o áudio do preview.")
endif()

add_executable(pierrot ${PIERROT_APP_SOURCES} ${PIERROT_APP_HEADERS})

target_include_directories(pierrot PRIVATE src src/colombina/ofx/sdk)

target_link_libraries(pierrot PRIVATE
    colombina
    ${PIERROT_QT_LIB}
    ${PIERROT_QT_MULTIMEDIA}
    ${PIERROT_QT_SVG}
)

# ── Testes (Fase 0) ─────────────────────────────────────────────────────
# Suíte de testes via Qt Test (sem dependência nova). Cobre a lógica pura do
# modelo (serialização .Blanc, interpolação de keyframes) e os caminhos de
# erro do exportador, que não tocam o sistema de arquivos nem o ffmpeg.

enable_testing()

# tst_core: lógica pura do modelo — só precisa de Project.cpp + Qt Core/Gui.
# Cada arquivo usa QTEST_APPLESS_MAIN (define main), então cada um vira um
# executável próprio para não colidir o símbolo main.
add_executable(tst_keyframes
    tests/tst_keyframes.cpp
    src/colombina/models/Project.cpp
)
target_include_directories(tst_keyframes PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_keyframes PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_keyframes COMMAND tst_keyframes)

add_executable(tst_serialization
    tests/tst_serialization.cpp
    src/colombina/models/Project.cpp
)
target_include_directories(tst_serialization PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_serialization PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_serialization COMMAND tst_serialization)

add_executable(tst_edl
    tests/tst_edl.cpp
    src/colombina/export/NleInterchange.cpp
    src/colombina/models/Project.cpp
)
target_include_directories(tst_edl PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_edl PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_edl COMMAND tst_edl)

# tst_exporter: caminhos de erro do ProjectExporter::buildCommand. Linka a
# pilha mínima de exportação (Project + FFmpegDecoder + MesaRenderer +
# ProjectExporter) + libs FFmpeg, sem UI e sem o ffmpeg CLI rodando de verdade.
add_executable(tst_exporter
    tests/tst_exporter.cpp
    src/colombina/models/Project.cpp
    src/colombina/ffmpeg/FFmpegDecoder.cpp
    src/colombina/render/MesaRenderer.cpp
    src/colombina/export/ProjectExporter.cpp
    src/colombina/ffmpeg/ProxyManager.cpp
)
target_include_directories(tst_exporter PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_exporter PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    ${PIERROT_QT_CONCURRENT}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_exporter COMMAND tst_exporter)

install(TARGETS pierrot RUNTIME DESTINATION bin)
install(FILES packaging/pierrot.desktop DESTINATION share/applications)
install(FILES packaging/pierrot.svg DESTINATION share/icons/hicolor/scalable/apps)
