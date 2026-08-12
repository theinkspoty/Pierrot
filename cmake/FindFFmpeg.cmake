# FindFFmpeg.cmake
#
# Localiza as bibliotecas libavformat, libavcodec, libavutil, libswscale (e,
# opcionalmente, libswresample).
#
# Estratégia:
#   1. Se pkg-config estiver disponível, usa diretamente os includes e libs
#      reportados (libavformat.pc etc.) — padrão em Linux/Arch.
#   2. Fallback: find_path/find_library (funciona no Windows com vcpkg).
#      Aponte FFMPEG_ROOT (cache) se as libs não estiverem em local padrão.
#
# Targets exportados: FFmpeg::avformat, FFmpeg::avcodec, FFmpeg::avutil,
# FFmpeg::swscale, FFmpeg::swresample (se encontrado).

find_package(PkgConfig QUIET)

set(_FFMPEG_COMPONENTS avformat avcodec avutil swscale swresample)

foreach(c IN LISTS _FFMPEG_COMPONENTS)
    string(TOUPPER ${c} UC)
    set(FFMPEG_${UC}_INCLUDE_DIR)
    set(FFMPEG_${UC}_LIBRARY)

    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_FFMPEG_${c} QUIET lib${c})
        if(PC_FFMPEG_${c}_FOUND)
            set(FFMPEG_${UC}_INCLUDE_DIR ${PC_FFMPEG_${c}_INCLUDE_DIRS})
            if(PC_FFMPEG_${c}_LINK_LIBRARIES)
                list(GET PC_FFMPEG_${c}_LINK_LIBRARIES 0 FFMPEG_${UC}_LIBRARY)
            endif()
        endif()
    endif()

    if(NOT FFMPEG_${UC}_INCLUDE_DIR OR NOT FFMPEG_${UC}_LIBRARY)
        if(DEFINED FFMPEG_ROOT)
            set(_ffmpeg_root_inc ${FFMPEG_ROOT}/include)
            set(_ffmpeg_root_lib ${FFMPEG_ROOT}/lib)
        else()
            set(_ffmpeg_root_inc)
            set(_ffmpeg_root_lib)
        endif()
        find_path(FFMPEG_${UC}_INCLUDE_DIR
            NAMES ${c}/${c}.h
            HINTS ${_ffmpeg_root_inc}
            PATH_SUFFIXES include)
        find_library(FFMPEG_${UC}_LIBRARY
            NAMES ${c}
            HINTS ${_ffmpeg_root_lib})
    endif()
endforeach()

set(FFmpeg_REQUIRED_VARS)
foreach(c avformat avcodec avutil swscale)
    string(TOUPPER ${c} UC)
    list(APPEND FFmpeg_REQUIRED_VARS FFMPEG_${UC}_INCLUDE_DIR FFMPEG_${UC}_LIBRARY)
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    DEFAULT_MSG
    ${FFmpeg_REQUIRED_VARS})

mark_as_advanced(${FFmpeg_REQUIRED_VARS} FFMPEG_SWRESAMPLE_INCLUDE_DIR FFMPEG_SWRESAMPLE_LIBRARY)

if(FFmpeg_FOUND)
    foreach(c IN LISTS _FFMPEG_COMPONENTS)
        string(TOUPPER ${c} UC)
        if(FFMPEG_${UC}_LIBRARY AND FFMPEG_${UC}_INCLUDE_DIR AND NOT TARGET FFmpeg::${c})
            add_library(FFmpeg::${c} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${c} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_${UC}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${UC}_INCLUDE_DIR}")
        endif()
    endforeach()
endif()
