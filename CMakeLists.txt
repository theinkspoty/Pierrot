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

# ── LaArtman: o motor de codecs, a camada mais baixa ───────────────────
# Só I/O de mídia via libav (decode hoje; encode/mux no futuro). Não conhece
# Project, render nem UI — é deliberadamente a base de tudo. A colombina
# depende dele; ninguém acima o inclui direto além de incluir seu header.
set(LAARTMAN_SOURCES
    src/laartman/FFmpegDecoder.cpp
    src/laartman/FFmpegEncoder.cpp
)

set(LAARTMAN_HEADERS
    src/laartman/FFmpegDecoder.h
    src/laartman/FFmpegEncoder.h
)

# ── Colombina: o kernel, a camada blindada (quase não muda) ─────────────
# models/, ffmpeg/, render/, export/, ofx/. Não pode incluir src/ui — a
# barreira é estrutural: nada aqui pode depender da casca (widgets).
set(COLOMBINA_SOURCES
    src/colombina/models/Project.cpp
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
    src/colombina/frei0r/Frei0rPluginManager.cpp
    src/colombina/mesh/ObjLoader.cpp
    src/colombina/mesh/BlenderBridge.cpp
    src/colombina/render/MesaRenderer.cpp
    src/colombina/CrashReporter.cpp
)

set(COLOMBINA_HEADERS
    src/colombina/models/Project.h
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
    src/colombina/frei0r/Frei0rPluginManager.h
    src/colombina/mesh/ObjLoader.h
    src/colombina/mesh/BlenderBridge.h
    src/colombina/fx/ColorGrade.h
    src/colombina/render/MesaRenderer.h
    src/colombina/render/Math3D.h
    src/colombina/generators.h
    src/colombina/util.h
    src/colombina/CrashReporter.h
)

# ── Casca: a UI (muda o tempo todo) ────────────────────────────────────
set(PIERROT_APP_SOURCES
    src/main.cpp
    src/Bench.cpp
    src/MainWindow.cpp
    src/ui/MediaPoolWidget.cpp
    src/ui/TimelineWidget.cpp
    src/ui/TimelineCommands.cpp
    src/ui/TimelineSelection.cpp
    src/ui/TimelinePaint.cpp
    src/ui/TimelineDrag.cpp
    src/ui/PlaybackEngine.cpp
    src/ui/PreviewProfiler.cpp
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
    src/ui/SourceMonitorWidget.cpp
    src/ui/VelocityEditorWidget.cpp
    src/ui/PivotWidget.cpp
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
    src/Bench.h
    src/clipattrs.h
    src/version.h
    src/ui/TrimmerDialog.h
    src/ui/SourceMonitorWidget.h
    src/ui/VelocityEditorWidget.h
    src/ui/PivotWidget.h
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

# ── DSP em Rust (opcional): portada de AudioFx (EQ/gate/AGC/reverb) ─────
# Off por padrão; ligue com `-DPIERROT_ENABLE_RUST=ON` após instalar o Rust.
# A lib pierrot_audiofx é staticlib via cargo; o header expõe a fronteira C.
option(PIERROT_ENABLE_RUST "Usar o DSP de áudio portado para Rust" OFF)

if(PIERROT_ENABLE_RUST)
    find_program(CARGO_EXECUTABLE cargo)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "PIERROT_ENABLE_RUST=ON mas cargo não foi encontrado. Instale o Rust (https://rustup.rs).")
    endif()
    if(NOT CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
        message(WARNING "DSP Rust sem -O3 pode engasgar no áudio; use Release ou RelWithDebInfo.")
    endif()

    set(PIERROT_RUST_TARGET pierrot_audiofx)
    set(PIERROT_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/rust/pierrot_audiofx")
    set(PIERROT_RUST_BUILD "${CMAKE_CURRENT_BINARY_DIR}/rust/${PIERROT_RUST_TARGET}")
    set(PIERROT_RUST_RELEASE "${PIERROT_RUST_BUILD}/target/release")
    set(PIERROT_RUST_LIB "${PIERROT_RUST_RELEASE}/lib${PIERROT_RUST_TARGET}.a")

    # make muda para WORKING_DIRECTORY ANTES de rodar os comandos, então o
    # diretório precisa existir já na configuração (não dentro do comando).
    file(MAKE_DIRECTORY ${PIERROT_RUST_BUILD})

    add_custom_command(
        OUTPUT ${PIERROT_RUST_LIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PIERROT_RUST_BUILD}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PIERROT_RUST_DIR} ${PIERROT_RUST_BUILD}
        COMMAND ${CARGO_EXECUTABLE} build --release --offline
                WORKING_DIRECTORY ${PIERROT_RUST_BUILD}
        DEPENDS ${PIERROT_RUST_DIR}/Cargo.toml
                ${PIERROT_RUST_DIR}/src/lib.rs
    )
    add_custom_target(pierrot_audiofx ALL DEPENDS ${PIERROT_RUST_LIB})

    add_library(pierrot_rust STATIC IMPORTED GLOBAL)
    set_target_properties(pierrot_rust PROPERTIES
        IMPORTED_LOCATION ${PIERROT_RUST_LIB}
        INTERFACE_INCLUDE_DIRECTORIES "${PIERROT_RUST_DIR}/include"
    )
    add_dependencies(pierrot_rust pierrot_audiofx)
    list(APPEND PIERROT_APP_HEADERS src/ui/AudioFxBridge.h)
endif()

# ── Alvos ───────────────────────────────────────────────────────────────
add_library(laartman STATIC ${LAARTMAN_SOURCES} ${LAARTMAN_HEADERS})

# `src` no include path para os consumidores acharem "laartman/FFmpegDecoder.h".
target_include_directories(laartman PUBLIC src)

target_link_libraries(laartman PUBLIC
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
)

if(TARGET FFmpeg::swresample)
    target_link_libraries(laartman PUBLIC FFmpeg::swresample)
else()
    message(FATAL_ERROR "FFmpeg libswresample não encontrada — necessária para decodificar o áudio.")
endif()

add_library(colombina STATIC ${COLOMBINA_SOURCES} ${COLOMBINA_HEADERS})

target_include_directories(colombina PUBLIC src src/colombina/ofx/sdk)

target_link_libraries(colombina PUBLIC
    laartman
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

if(PIERROT_ENABLE_RUST)
    target_link_libraries(pierrot PRIVATE pierrot_rust)
    target_compile_definitions(pierrot PRIVATE PIERROT_ENABLE_RUST=1)
endif()

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

# tst_math3d: Fase 0 da Mesa 3D — Vec3/Mat4 + campos mesa3d serializados.
add_executable(tst_math3d
    tests/tst_math3d.cpp
    src/colombina/models/Project.cpp
)
target_include_directories(tst_math3d PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_math3d PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_math3d COMMAND tst_math3d)

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
    src/laartman/FFmpegDecoder.cpp
    src/colombina/render/MesaRenderer.cpp
    src/colombina/export/ProjectExporter.cpp
    src/colombina/ffmpeg/ProxyManager.cpp
    src/colombina/frei0r/Frei0rPluginManager.cpp
    src/colombina/mesh/ObjLoader.cpp
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

# tst_audio_conform: contrato do AudioConformCache consumido pelo MixerWidget —
# leitura posicional de um PCM com lacunas (regiões ainda não descodificadas
# saem em silêncio e truncam a leitura) e refcounting manual do buffer. Os
# ficheiros de áudio não existem de propósito: o worker só descodifica quando
# há janelas pedidas, e o teste nunca pede nenhuma. Linka a pilha de FFmpeg
# porque o .cpp chama o decoder, mas não chega a abrí-lo.
# tst_crashreporter: o relatório de crash tem que listar as threads e o que
# elas estavam fazendo. Faz fork e mata o filho com SIGSEGV de verdade, então
# roda isolado e rápido. Precisa do CrashReporter compilado junto.
add_executable(tst_crashreporter
    tests/tst_crashreporter.cpp
    src/colombina/CrashReporter.cpp
)
target_include_directories(tst_crashreporter PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_crashreporter PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
)
add_test(NAME tst_crashreporter COMMAND tst_crashreporter)

# tst_concurrency: exercita de verdade os caminhos serializados do
# FFmpegDecoder (frameAt concorrente, close() sobre decode em andamento,
# accessors de áudio durante open, dois decoders no mesmo arquivo). Gera
# mídia com o ffmpeg CLI; sem ffmpeg no PATH pula. É o que dá dente ao job de
# ASAN/UBSAN — a cobertura do decoder era 2,3% e a do MesaRenderer 0%.
add_executable(tst_concurrency
    tests/tst_concurrency.cpp
    src/laartman/FFmpegDecoder.cpp
)
target_include_directories(tst_concurrency PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_concurrency PRIVATE
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
add_test(NAME tst_concurrency COMMAND tst_concurrency)

# tst_encoder: ciclo encode→mux→decode do motor LaArtman. Gera quadros RGBA,
# codifica num MP4 (libx264) e decodifica de volta; pula se o FFmpeg não tiver
# o encoder. Linka a lib `laartman` de verdade (valida o alvo, não duplica .cpp).
add_executable(tst_encoder
    tests/tst_encoder.cpp
)
target_include_directories(tst_encoder PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_encoder PRIVATE
    laartman
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_encoder COMMAND tst_encoder)

add_executable(tst_audio_conform
    tests/tst_audio_conform.cpp
    src/colombina/ffmpeg/AudioConformCache.cpp
    src/laartman/FFmpegDecoder.cpp
)
target_include_directories(tst_audio_conform PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_audio_conform PRIVATE
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
add_test(NAME tst_audio_conform COMMAND tst_audio_conform)

# tst_audio_conform_intervals: a álgebra pura de intervalos/janelas que o
# AudioConformCache usa. Header-only, por isso não linka FFmpeg nem precisa
# de pthread — compila e corre num instante, ao contrário do tst_audio_conform.
add_executable(tst_audio_conform_intervals
    tests/tst_audio_conform_intervals.cpp
)
target_include_directories(tst_audio_conform_intervals PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_audio_conform_intervals PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
)
add_test(NAME tst_audio_conform_intervals COMMAND tst_audio_conform_intervals)

# tst_export_pipeline: CAMINHO FELIZ — roda o ffmpeg CLI de verdade com o comando
# do ProjectExporter e extrai um quadro do arquivo resultante, comparando com o
# generatorFrame() (mesma fonte de verdade do preview). Pulado se não houver
# ffmpeg no PATH. Mantém a paridade preview ↔ exportação verificável no CI.
add_executable(tst_export_pipeline
    tests/tst_export_pipeline.cpp
    src/colombina/models/Project.cpp
    src/laartman/FFmpegDecoder.cpp
    src/colombina/render/MesaRenderer.cpp
    src/colombina/export/ProjectExporter.cpp
    src/colombina/ffmpeg/ProxyManager.cpp
    src/colombina/frei0r/Frei0rPluginManager.cpp
    src/colombina/mesh/ObjLoader.cpp
)
target_include_directories(tst_export_pipeline PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_export_pipeline PRIVATE
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
add_test(NAME tst_export_pipeline COMMAND tst_export_pipeline)

install(TARGETS pierrot RUNTIME DESTINATION bin)
install(FILES packaging/pierrot.desktop DESTINATION share/applications)
install(FILES packaging/pierrot.svg DESTINATION share/icons/hicolor/scalable/apps)
