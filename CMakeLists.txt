cmake_minimum_required(VERSION 3.24)
project(klip_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFMPEG_ROOT "" CACHE PATH "Path to FFmpeg root containing include/ and lib/")
if(NOT FFMPEG_ROOT)
  message(FATAL_ERROR "Set FFMPEG_ROOT to your FFmpeg build root.")
endif()

find_path(FFMPEG_INCLUDE_DIR
  NAMES libavcodec/avcodec.h
  PATHS "${FFMPEG_ROOT}/include"
  NO_DEFAULT_PATH)

find_library(FFMPEG_AVCODEC_LIB
  NAMES avcodec
  PATHS "${FFMPEG_ROOT}/lib"
  NO_DEFAULT_PATH)

find_library(FFMPEG_AVFORMAT_LIB
  NAMES avformat
  PATHS "${FFMPEG_ROOT}/lib"
  NO_DEFAULT_PATH)

find_library(FFMPEG_AVUTIL_LIB
  NAMES avutil
  PATHS "${FFMPEG_ROOT}/lib"
  NO_DEFAULT_PATH)

find_library(FFMPEG_SWRESAMPLE_LIB
  NAMES swresample
  PATHS "${FFMPEG_ROOT}/lib"
  NO_DEFAULT_PATH)

if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC_LIB OR NOT FFMPEG_AVFORMAT_LIB OR NOT FFMPEG_AVUTIL_LIB OR NOT FFMPEG_SWRESAMPLE_LIB)
  message(FATAL_ERROR "Could not locate FFmpeg headers/libs under FFMPEG_ROOT.")
endif()

file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")

find_package(imgui CONFIG REQUIRED
  PATHS "C:/vcpkg/installed/x64-windows/share/imgui"
  NO_DEFAULT_PATH)

add_executable(klip_core WIN32
  src/main.cpp)

target_include_directories(klip_core PRIVATE
  include
  "${FFMPEG_INCLUDE_DIR}")

target_compile_definitions(klip_core PRIVATE
  WINRT_LEAN_AND_MEAN
  UNICODE
  _UNICODE
  NOMINMAX)

target_link_libraries(klip_core PRIVATE
  imgui::imgui
  # FFmpeg
  "${FFMPEG_AVCODEC_LIB}"
  "${FFMPEG_AVFORMAT_LIB}"
  "${FFMPEG_AVUTIL_LIB}"
  "${FFMPEG_SWRESAMPLE_LIB}"
  # Win32 / DirectX / WinRT interop
  avrt
  d3d11
  dxgi
  dwmapi
  mmdevapi
  ole32
  propsys
  uuid
  windowsapp
  runtimeobject)

if(FFMPEG_RUNTIME_DLLS)
  add_custom_command(TARGET klip_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${FFMPEG_RUNTIME_DLLS}
      $<TARGET_FILE_DIR:klip_core>)
endif()
