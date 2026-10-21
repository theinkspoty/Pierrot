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

# ── Relatórios de crash detalhados ─────────────────────────────────────
# O CrashReporter resolve nomes de função via dladdr(), o que só funciona se o
# executável exportar os próprios símbolos (ENABLE_EXPORTS → -rdynamic). O
# frame pointer torna o backtrace confiável e -g deixa o addr2line achar
# arquivo/linha. Commit e hora do build vão gravados no cabeçalho do relatório.
option(PIERROT_CRASH_DEBUG "Símbolos/frame pointers para relatórios de crash detalhados" ON)
if(PIERROT_CRASH_DEBUG)
    add_compile_options(-fno-omit-frame-pointer -g)
endif()

# ── Sanitizers (aceitação da reestruturação, Fase 8b) ──────────────────
# Liga AddressSanitizer + UndefinedBehaviorSanitizer em todos os alvos
# (compile E link). UBSan aborta no primeiro erro (-fno-sanitize-recover)
# para que o ctest falhe visível em vez de só imprimir um aviso. Uso:
#   cmake -S . -B build-asan -DPIERROT_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-asan -j$(nproc)
#   ctest --test-dir build-asan --output-on-failure
option(PIERROT_SANITIZE "Compilar com ASan+UBSan (Fase 8b)" OFF)
if(PIERROT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Pierrot: ASan+UBSan habilitados")
endif()

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE PIERROT_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT PIERROT_GIT_COMMIT)
    set(PIERROT_GIT_COMMIT "desconhecido")
endif()
string(TIMESTAMP PIERROT_BUILD_TIME "%Y-%m-%d %H:%M:%S")

# ── LaArtman: o motor de codecs, a camada mais baixa ───────────────────
# Só I/O de mídia via libav (decode hoje; encode/mux no futuro). Não conhece
# Project, render nem UI — é deliberadamente a base de tudo. A colombina
# depende dele; ninguém acima o inclui direto além de incluir seu header.
set(LAARTMAN_SOURCES
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
)

set(LAARTMAN_HEADERS
    src/laartman/Decoder.h
    src/laartman/core/FFmpegDecoder.h
    src/laartman/io/Demuxer.h
    src/laartman/io/PacketReader.h
    src/laartman/core/ErrorConceal.h
    src/laartman/core/DecoderMetrics.h
)

# ── Colombina: o kernel, a camada blindada (quase não muda) ─────────────
# models/, ffmpeg/, render/, export/, ofx/. Não pode incluir src/ui — a
# barreira é estrutural: nada aqui pode depender da casca (widgets).
set(COLOMBINA_SOURCES
    src/colombina/models/Project.cpp
    src/colombina/ffmpeg/AudioConformCache.cpp
    src/colombina/ffmpeg/MediaCache.cpp
    src/colombina/ffmpeg/ProxyManager.cpp
    src/colombina/ffmpeg/SourcePool.cpp
    src/colombina/ffmpeg/FramePipeline.cpp
    src/colombina/ffmpeg/FrameCache.cpp
    src/colombina/ffmpeg/FFmpegEncoder.cpp
    src/colombina/export/ProjectExporter.cpp
    src/colombina/export/LainkaRenderer.cpp
    src/colombina/export/OfxExportRenderer.cpp
    src/colombina/export/PreRenderSession.cpp
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
    src/colombina/models/ClipAttrs.h
    src/colombina/ffmpeg/MediaCache.h
    src/colombina/ffmpeg/ProxyManager.h
    src/colombina/ffmpeg/SourcePool.h
    src/colombina/ffmpeg/FramePipeline.h
    src/colombina/ffmpeg/FrameCache.h
    src/colombina/ffmpeg/FFmpegEncoder.h
    src/colombina/export/ProjectExporter.h
    src/colombina/export/LainkaFx.h
    src/colombina/export/LainkaRenderer.h
    src/colombina/export/OfxExportRenderer.h
    src/colombina/export/PreRenderSession.h
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
    src/colombina/render/TextRender.h
    src/colombina/generators.h
    src/colombina/util.h
    src/colombina/CrashReporter.h
)

# ── Casca: a UI (muda o tempo todo) ────────────────────────────────────
set(PIERROT_APP_SOURCES
    src/main.cpp
    src/Bench.cpp
    src/MainWindow.cpp
    src/MainWindow.Icons.cpp
    src/ui/panels/MediaPoolWidget.cpp
    src/ui/timeline/TimelineWidget.cpp
    src/ui/timeline/TimelineCommands.cpp
    src/ui/timeline/TimelineSelection.cpp
    src/ui/timeline/TimelinePaint.cpp
    src/ui/timeline/TimelineDrag.cpp
    src/ui/monitor/PlaybackEngine.cpp
    src/ui/monitor/PreviewProfiler.cpp
    src/ui/monitor/PreviewWidget.cpp
    src/ui/monitor/FrameWorker.cpp
    src/ui/monitor/MonitorScrubBar.cpp
    src/ui/timeline/TlLog.cpp
    src/ui/monitor/UiDecode.cpp
    src/ui/mesa/PantaloneWidget.cpp
    src/ui/dialogs/ExportDialog.cpp
    src/ui/dialogs/ProjectSettingsDialog.cpp
    src/ui/dialogs/SettingsDialog.cpp
    src/ui/core/Theme.cpp
    src/ui/core/TechnicalWindow.cpp
    src/ui/core/JaTranslator.cpp
    src/ui/dialogs/TransformDialog.cpp
    src/ui/dialogs/MaskEditorDialog.cpp
    src/ui/audio/AudioEffectsDialog.cpp
    src/ui/audio/TrackAudioFxDialog.cpp
    src/ui/dialogs/TextEditorDialog.cpp
    src/ui/motion/LazziDock.cpp
    src/ui/motion/AnimPresetDock.cpp
    src/ui/panels/ColorWheel.cpp
    src/ui/panels/ColorDock.cpp
    src/ui/dialogs/WelcomeWindow.cpp
    src/ui/motion/GraphEditorWidget.cpp
    src/ui/timeline/ClipPropertiesWidget.cpp
    src/ui/core/ClickLogger.cpp
    src/ui/panels/EffectsWidget.cpp
    src/ui/panels/ExpressWidget.cpp
    src/ui/monitor/TrimmerDialog.cpp
    src/ui/monitor/SourceMonitorWidget.cpp
    src/ui/motion/VelocityEditorWidget.cpp
    src/ui/mesa/PivotWidget.cpp
    src/ui/dialogs/RenderQueueDialog.cpp
    src/ui/monitor/ScopeWidget.cpp
    src/ui/monitor/PreviewMonitor.cpp
    src/ui/panels/FileBrowserWidget.cpp
    src/ui/audio/MixerWidget.cpp
    src/ui/audio/AudioFxDsp.cpp
    src/ui/audio/AudioMixer.cpp
    src/ui/audio/EqualizerWidget.cpp
    src/ui/audio/EqCurveView.cpp
    src/ui/mesa/MesaWidget.cpp
    src/ui/mesa/MesaHitTest.cpp
    src/ui/mesa/MesaInputHandler.cpp
    src/ui/mesa/MesaOverlayRenderer.cpp
    src/ui/mesa/MesaCanvasOps.cpp
    resources.qrc
)

set(PIERROT_APP_HEADERS
    src/MainWindow.h
    src/Bench.h
    src/version.h
    src/ui/monitor/TrimmerDialog.h
    src/ui/monitor/SourceMonitorWidget.h
    src/ui/motion/VelocityEditorWidget.h
    src/ui/mesa/PivotWidget.h
    src/ui/monitor/UiDecode.h
    src/ui/dialogs/RenderQueueDialog.h
    src/ui/monitor/ScopeWidget.h
    src/ui/panels/MediaPoolWidget.h
    src/ui/timeline/TimelineWidget.h
    src/ui/timeline/TimelineCommands.h
    src/ui/monitor/PreviewWidget.h
    src/ui/monitor/PreviewDebug.h
    src/ui/monitor/FrameWorker.h
    src/ui/monitor/MonitorScrubBar.h
    src/ui/monitor/PlaybackEngine.h
    src/ui/monitor/PreviewMonitor.h
    src/ui/monitor/MonitorUtils.h
    src/ui/mesa/PantaloneWidget.h
    src/ui/dialogs/ExportDialog.h
    src/ui/dialogs/ProjectSettingsDialog.h
    src/ui/dialogs/SettingsDialog.h
    src/ui/core/Theme.h
    src/ui/core/TechnicalWindow.h
    src/ui/dialogs/TransformDialog.h
    src/ui/dialogs/MaskEditorDialog.h
    src/ui/audio/AudioEffectsDialog.h
    src/ui/audio/TrackAudioFxDialog.h
    src/ui/audio/AudioFxHelpers.h
    src/ui/audio/AudioFxDsp.h
    src/ui/audio/AudioMixer.h
    src/ui/dialogs/TextEditorDialog.h
    src/ui/motion/LazziDock.h
    src/ui/motion/AnimPresetDock.h
    src/ui/panels/ColorWheel.h
    src/ui/panels/ColorDock.h
    src/ui/dialogs/WelcomeWindow.h
    src/ui/motion/GraphEditorWidget.h
    src/ui/timeline/ClipPropertiesWidget.h
    src/ui/core/ClickLogger.h
    src/ui/panels/EffectsWidget.h
    src/ui/panels/ExpressWidget.h
    src/ui/panels/FileBrowserWidget.h
    src/ui/audio/MixerWidget.h
    src/ui/audio/EqualizerWidget.h
    src/ui/audio/EqCurveView.h
    src/ui/mesa/MesaWidget.h
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
    list(APPEND PIERROT_APP_HEADERS src/ui/audio/AudioFxBridge.h)
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

# Commit/hora do build aparecem no relatório de crash (CrashReporter.cpp).
target_compile_definitions(colombina PRIVATE
    PIERROT_GIT_COMMIT="${PIERROT_GIT_COMMIT}"
    PIERROT_BUILD_TIME="${PIERROT_BUILD_TIME}")

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

# -rdynamic: exporta os símbolos internos para o CrashReporter (dladdr).
set_target_properties(pierrot PROPERTIES ENABLE_EXPORTS ON)

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
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
    src/colombina/ffmpeg/SourcePool.cpp
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
# -rdynamic para o relatório resolver os símbolos do próprio teste.
set_target_properties(tst_crashreporter PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(tst_crashreporter PRIVATE
    PIERROT_GIT_COMMIT="${PIERROT_GIT_COMMIT}"
    PIERROT_BUILD_TIME="${PIERROT_BUILD_TIME}")
target_include_directories(tst_crashreporter PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_crashreporter PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_crashreporter COMMAND tst_crashreporter)

# tst_concurrency: exercita de verdade os caminhos serializados do
# FFmpegDecoder (frameAt concorrente, close() sobre decode em andamento,
# accessors de áudio durante open, dois decoders no mesmo arquivo). Gera
# mídia com o ffmpeg CLI; sem ffmpeg no PATH pula. É o que dá dente ao job de
# ASAN/UBSAN — a cobertura do decoder era 2,3% e a do MesaRenderer 0%.
add_executable(tst_concurrency
    tests/tst_concurrency.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
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

# tst_decoder_api: CONTRATO da API pública do núcleo L3 (fachada Decoder do
# plano v0.4 + o motor FFmpegDecoder). Dá dente aos invariantes da seção 9 do
# relatório: determinismo/paridade de frame (mesmo path,t,maxW → pixels
# idênticos, em qualquer ordem, antes/depois de reabrir), idempotência (close
# 2×, open já aberto, frame após close), a LINHA DE BASE de latência do caminho
# quente (p99 ≈ p50) e o encapsulamento (o header público não reexpõe
# posse/política). Gera mídia com o ffmpeg CLI; sem ffmpeg no PATH, só o
# caminho de falha do open() roda.
add_executable(tst_decoder_api
    tests/tst_decoder_api.cpp
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
    src/colombina/ffmpeg/FFmpegEncoder.cpp
)
# O teste lê o próprio header público como texto (guarda de encapsulamento).
target_compile_definitions(tst_decoder_api PRIVATE
    PIERROT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(tst_decoder_api PRIVATE src)
target_link_libraries(tst_decoder_api PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_decoder_api COMMAND tst_decoder_api)

# tst_realmedia: fixture de MÍDIA REAL (lacuna do relatório de estresse §6):
# varre <raiz>/tests/media e valida probe/frames/determinismo em cada arquivo
# (VFR de câmera, MPEG-TS, vertical 1080, GIF longo, AV1 real…). Diretório
# vazio/inexistente → todos os casos pulam (CI não quebra); popule o
# diretório para ativá-lo. Isolado de GPU (lógica do motor, não hw).
add_executable(tst_realmedia
    tests/tst_realmedia.cpp
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
)
target_compile_definitions(tst_realmedia PRIVATE
    TEST_MEDIA_DIR="${CMAKE_SOURCE_DIR}/tests/media")
target_include_directories(tst_realmedia PRIVATE src)
target_link_libraries(tst_realmedia PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_realmedia COMMAND tst_realmedia)
set_tests_properties(tst_realmedia PROPERTIES ENVIRONMENT "PIERROT_GPU=0")

# tst_sourcepool: acesso compartilhado/LRU/eviction do pool de decoders quentes
# (política L4, colombina/ffmpeg/SourcePool). Garantia central: acquire() NUNCA
# devolve nullptr por "ocupado". Sem ffmpeg no PATH só o caminho de falha do
# open roda; com ele gera clipes sintéticos e exercita reuse/eviction/concorrência.
add_executable(tst_sourcepool
    tests/tst_sourcepool.cpp
    src/colombina/ffmpeg/SourcePool.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
)
target_include_directories(tst_sourcepool PRIVATE src)
target_link_libraries(tst_sourcepool PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_sourcepool COMMAND tst_sourcepool)

# tst_framepipeline: política de lookahead L4 (colombina/ffmpeg/FramePipeline).
# Dono da thread de prefetch, da fila de warm e do estado busy — o que saiu do
# PreviewWidget na Fase 2. Verifica que a fila é substituída (não acumula), que
# o prefetch de frame0 é serial e devolve o par (t, t+step), que warmNow aquece
# o pool e limpa o in-flight, e que stop() é seguro/idempotente. Sem ffmpeg no
# PATH só o caminho sem mídia roda.
add_executable(tst_framepipeline
    tests/tst_framepipeline.cpp
    src/colombina/ffmpeg/FramePipeline.cpp
    src/colombina/ffmpeg/SourcePool.cpp
    src/colombina/CrashReporter.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
)
target_include_directories(tst_framepipeline PRIVATE src)
target_compile_definitions(tst_framepipeline PRIVATE
    PIERROT_GIT_COMMIT="${PIERROT_GIT_COMMIT}"
    PIERROT_BUILD_TIME="${PIERROT_BUILD_TIME}")
target_link_libraries(tst_framepipeline PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_framepipeline COMMAND tst_framepipeline)

# tst_packetreader: leitura de pacotes à frente com buffer limitado (L0).
# Verifica que o teto é respeitado (memória O(bucket)), que a leitura esgota os
# pacotes até o EOF, que seek() reposiciona, que close() é idempotente e que
# cancel aborta a espera. Sem ffmpeg no PATH só o caminho de falha do open roda.
add_executable(tst_packetreader
    tests/tst_packetreader.cpp
    src/laartman/io/PacketReader.cpp
)
target_include_directories(tst_packetreader PRIVATE src)
target_link_libraries(tst_packetreader PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_packetreader COMMAND tst_packetreader)

# tst_errorconceal: tabela de decisão da ocultação de erro (L1). Erro de decode
# ou fim no meio de um GOP com quadro bom → Concealed; sem quadro bom → Error/
# Eof (nunca silêncio). Usa AVFrame sintéticos (sem mídia).
add_executable(tst_errorconceal
    tests/tst_errorconceal.cpp
)
target_include_directories(tst_errorconceal PRIVATE src)
target_link_libraries(tst_errorconceal PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    FFmpeg::avcodec
    FFmpeg::avutil
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_errorconceal COMMAND tst_errorconceal)

# tst_framecache: LRU de quadros L4 (colombina/ffmpeg/FrameCache). O cache saiu
# do núcleo na Fase 6. Verifica miss/hit, move-to-front, evicção LRU, isolamento
# entre chaves e que imagem nula não é inserida. Qt puro, sem ffmpeg.
add_executable(tst_framecache
    tests/tst_framecache.cpp
    src/colombina/ffmpeg/FrameCache.cpp
)
target_include_directories(tst_framecache PRIVATE src)
target_link_libraries(tst_framecache PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
    ${CMAKE_DL_LIBS}
)
add_test(NAME tst_framecache COMMAND tst_framecache)

# tst_audiofx_dsp: invariantes do DSP do preview (AudioFxDsp extraído no F4):
# identidade de biquad 0 dB, passthrough neutro, ganho ↑ energia, clamp S16.
# Base para o futuro tst_audiofx_parity (C++ fallback ↔ Rust) do ROADMAP.
add_executable(tst_audiofx_dsp
    tests/tst_audiofx_dsp.cpp
)
target_include_directories(tst_audiofx_dsp PRIVATE src)
target_link_libraries(tst_audiofx_dsp PRIVATE
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_audiofx_dsp COMMAND tst_audiofx_dsp)

# tst_encoder: ciclo encode→mux→decode do motor de encode (agora em
# colombina/ffmpeg). Gera quadros RGBA, codifica num MP4 (libx264) e decodifica
# de volta; pula se o FFmpeg não tiver o encoder. Linka a lib `colombina`.
add_executable(tst_encoder
    tests/tst_encoder.cpp
)
target_include_directories(tst_encoder PRIVATE src src/colombina/ofx/sdk)
target_link_libraries(tst_encoder PRIVATE
    colombina
    ${PIERROT_QT_TEST}
    ${PIERROT_QT_CORE}
    ${PIERROT_QT_GUI}
)
add_test(NAME tst_encoder COMMAND tst_encoder)
# Isolamento de hw: estes testes cobrem LÓGICA (pool/LRU do SourcePool, fila
# de prefetch do FramePipeline, round-trip de encode) — não o caminho de GPU.
# Rodar com VAAPI desligado remove a contenção do driver NVIDIA das rodadas
# paralelas: o abort intermitente do tst_sourcepool na suíte só apareceu sob
# carga de GPU mista e nunca se reproduziu isolado (13 rodadas limpas em
# 2026-10-21). Os testes que COBREM hw (decoder_api, concurrency) seguem com
# VAAPI ligado.
set_tests_properties(tst_sourcepool tst_framepipeline tst_encoder
    PROPERTIES ENVIRONMENT "PIERROT_GPU=0")

add_executable(tst_audio_conform
    tests/tst_audio_conform.cpp
    src/colombina/ffmpeg/AudioConformCache.cpp
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
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
    src/laartman/Decoder.cpp
    src/laartman/core/FFmpegDecoder.cpp
    src/laartman/core/FFmpegDecoderOpen.cpp
    src/laartman/core/FFmpegDecoderAudio.cpp
    src/laartman/core/FFmpegDecoderProbe.cpp
    src/laartman/io/Demuxer.cpp
    src/laartman/io/PacketReader.cpp
    src/colombina/ffmpeg/FrameCache.cpp
    src/colombina/ffmpeg/SourcePool.cpp
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

# ── Sanitizers nos testes (Fase 8b) ────────────────────────────────────
# Com PIERROT_SANITIZE=ON, aplica a TODOS os testes os options do ASan/UBSan.
# O driver NVIDIA (libcuda/nvidia_drv_video) aloca globais uma única vez no
# cuInit e nunca libera — ruído de terceiro, não vazamento nosso. O
# suppressions=apaga esse ruído; halt_on_error=1 mantém o UBSan fatal.
if(PIERROT_SANITIZE)
    get_property(pierrot_all_tests DIRECTORY PROPERTY TESTS)
    if(pierrot_all_tests)
        # ASAN_OPTIONS não aceita supressões do tipo "leak:" (só tipos como
        # interceptor_name/odr_violation) — as entradas leak: do asan.supp
        # precisam ir em LSAN_OPTIONS, senão o parse falha e TODOS os testes
        # morrem antes de rodar.
        # fast_unwind_on_malloc=0: sem isso o unwind rápido do ASan corta a
        # pilha das alocações do driver (libEGL/nvidia) em "<unknown module>"
        # e as supressões leak: não casam — os testes de decoder morrem com
        # falso-positivo de vazamento de terceiro.
        set(_pirot_san_env
            "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:fast_unwind_on_malloc=0"
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/asan.supp"
            "UBSAN_OPTIONS=print_stacktrace=1")
        set_tests_properties(${pierrot_all_tests} PROPERTIES ENVIRONMENT "${_pirot_san_env}")
    endif()
endif()
