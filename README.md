# Cyberpunk Style Audio Visualiser

A Windows desktop overlay that reacts to low-frequency system audio with
bass-triggered cyberpunk glitch lines. It uses WASAPI loopback capture and a
DirectComposition/D3D11 transparent overlay.

## Requirements

- Windows 10 or later
- CMake 3.25 or later
- Visual Studio 2022 or later with the Desktop development with C++ workload
  and Windows SDK

## Build

From a Visual Studio Developer PowerShell in the project directory:

```powershell
cmake -S . -B build -G Ninja
cmake --build build --config Release
```

If Ninja is not installed, omit `-G Ninja` to use the default Visual Studio
generator.

## Run

Run `build\CyberpsychosisOverlay.exe` from the project directory so it can load
`glitch.hlsl`.

The overlay captures the default Windows render endpoint through WASAPI
loopback. It supports the endpoint's mix format, including 24-bit PCM.

## Hotkeys

- `Ctrl+Alt+T` — toggle mouse click-through
- `Ctrl+Alt+C` — toggle cel-shading
- `Ctrl+Alt+Q` — quit
