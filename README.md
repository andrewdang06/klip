# Klip

Klip is a Windows C++ low-latency game clipping app. It records gameplay into a rolling buffer and saves recent clips with a global hotkey.

## MVP Pipeline

The active app is currently implemented in `src/main.cpp` and uses:

- Windows Graphics Capture for video capture
- WASAPI loopback for system audio
- Optional WASAPI microphone capture
- FFmpeg encoders/muxers for H.264/AAC in MP4
- Separate capture, conversion, encode, audio, and clip-save work
- Bounded queues that drop frames when the encoder falls behind

Default hotkeys:

- `Alt+C`: save a clip
- `Alt+X`: hide/show the Klip window

Clips are written to `clips/` under the current working directory.

## Build Requirements

- Windows 10/11 with Windows Graphics Capture support
- Visual Studio 2022 C++ toolchain
- CMake 3.24+
- FFmpeg development build with `include/`, `lib/`, and `bin/`
- vcpkg `imgui` package installed for `x64-windows`

The current CMake file expects imgui at:

```powershell
C:/vcpkg/installed/x64-windows/share/imgui
```

Install imgui if needed:

```powershell
vcpkg install imgui[dx11-binding,win32-binding]:x64-windows
```

## Configure And Build

Point `FFMPEG_ROOT` at the FFmpeg folder that contains `include/`, `lib/`, and `bin/`.

```powershell
cmake -S . -B build -DFFMPEG_ROOT=C:/ffmpeg
cmake --build build --config Release
```

Runtime FFmpeg DLLs from `FFMPEG_ROOT/bin` are copied next to `klip_core.exe` after build.

## Encoder Setup

Klip asks FFmpeg for hardware encoders first, then falls back through other GPU paths. Current priority is:

1. NVIDIA: `h264_nvenc`
2. AMD: `h264_amf`
3. Intel/Windows Media Foundation: `h264_mf`, then QSV variants

Use an FFmpeg build that includes the encoder for your GPU. To verify:

```powershell
ffmpeg -hide_banner -encoders | findstr /i "h264_nvenc h264_amf h264_qsv h264_mf"
```

## Logs

Runtime logs are written to `klip.log`. Useful events include selected encoder, capture target changes, FFmpeg failures, clip save results, and capture/audio errors.
