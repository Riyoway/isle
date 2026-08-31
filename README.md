# Isle

A native, open-source dynamic activity island for Windows 10/11.

Isle sits at the top of the active monitor as a small always-on-top pill. It expands into a compact dashboard for media, system state, timers, quotas, and third-party activities. The shell is rendered with Direct2D + DirectComposition instead of a browser or transparent XAML window.

> Status: **0.1 engineering prototype**. The native renderer, spring geometry, system metrics, GSMTC media integration, tray controls, fullscreen policy, settings surface, and out-of-process plugin protocol are implemented. This repository is intentionally structured for the next iterations instead of being a one-file visual demo.

## Why this stack

The goal is a UI that is visually closer to iOS/macOS motion while still behaving like a good Windows overlay.

| Option | Good at | Problem for this app |
| --- | --- | --- |
| WinUI 3 | Windows controls, Composition APIs | top-level per-pixel window transparency is still an awkward/unsupported edge case; the exact overlay shape becomes the hard part |
| WPF | fast development, mature window APIs | transparent layered-window paths are not the rendering architecture I want for a permanent animated HUD |
| Tauri + React | easiest CSS motion and plugin UI | WebView2 is unnecessary for a tiny always-running surface and complicates native hit testing/resource goals |
| **C++23 + Win32 + D2D/DComp** | per-pixel premultiplied alpha, GPU composition, tiny shell, total window control | more engineering work, but it directly matches the problem |

DirectComposition can bind a premultiplied-alpha composition swap chain directly into the desktop composition tree, which is exactly what a transparent high-frequency HUD needs.

## Current features

- top-center, borderless, topmost activity pill
- per-monitor-v2 DPI awareness
- spring-based width/height/opacity transitions
- Direct2D + DirectWrite rendering on a DirectComposition swap chain
- collapsed, expanded, and settings states
- CPU, RAM, and battery/power metric rings
- Windows Global System Media Transport Controls (GSMTC) now-playing surface
- real album artwork, live timeline, animated waveform, and previous/play-pause/next actions
- system tray + global `Ctrl + Alt + Space` show/hide hotkey
- optional fullscreen auto-hide
- startup-with-Windows setting
- direct AI usage adapters (no external provider bridge)
- external plugin host over NDJSON stdio
- example usage-meter plugin that reproduces the 73 / 21 / 52 ring idea
- x64 + ARM64 CMake presets
- GPL-3.0 license and Windows CI

## Build

Requirements:

- Windows 10/11
- Visual Studio 2022 with **Desktop development with C++**
- a recent Windows 10/11 SDK with C++/WinRT headers
- CMake 3.28+

```powershell
cmake --preset windows-x64-release
cmake --build --preset release
```

The executable is produced under `build/x64-release/Release/Isle.exe`.

For an install-style output:

```powershell
cmake --install build/x64-release --config Release
```

## Plugin quick start

Plugins are not DLLs. Isle launches them as separate processes and speaks newline-delimited JSON over stdin/stdout. This keeps the renderer isolated and lets plugins be written in PowerShell, Python, Rust, Go, C#, Node, or anything else.

Install the bundled example:

```powershell
.\scripts\install-example-plugin.ps1
```

Restart Isle. The example publishes three usage meters. See [`plugin-sdk/protocol.md`](plugin-sdk/protocol.md) for the v1 protocol.

## Direct AI provider connections

Selected providers are queried directly from Isle. API keys may be supplied as
`ISLE_<PROVIDER>_API_KEY`, tokens as `ISLE_<PROVIDER>_TOKEN`, cookies as
`ISLE_<PROVIDER>_COOKIE`, or generic Windows credentials under `Isle/<provider>`.
Providers without a built-in endpoint can use `ISLE_<PROVIDER>_USAGE_URL`; the
response may contain percentage fields or a used/limit pair. Credentials stay
outside the settings file and are never written to logs.

## Controls

- click collapsed pill: expand
- click top header while expanded: collapse
- gear: open/close settings
- right click: context menu
- `Ctrl + Alt + Space`: show/hide
- tray icon left click: show/hide

## Project layout

```text
src/core       state, settings, provider interfaces, spring integrator
src/platform   HWND, Direct2D/DirectComposition renderer, tray, monitor placement
src/providers  built-in Windows integrations
src/plugins    out-of-process plugin broker
plugin-sdk     protocol, schemas, example plugin
```

## Design principles

1. **The island is a renderer, not a web page.** The permanent shell should be cheap and deterministic.
2. **Plugins describe state; the host owns the visual language.** This prevents a plugin from turning the UI into an inconsistent mini-browser.
3. **No focus stealing.** The overlay uses a non-activating tool window and mouse interactions do not take keyboard focus from the current app.
4. **No injected hooks for basic features.** Built-ins use public Windows APIs where practical.
5. **No auto-downloaded plugin code.** Third-party plugin installation is explicit.

## Next engineering targets

- true superellipse geometry + shadow cached as Direct2D command lists
- draggable file shelf with OLE drag/drop
- draggable GSMTC timeline
- volume/brightness quick controls
- notification listener provider with explicit permission flow
- monitor selection UI and edge placement modes
- plugin capability declarations and optional AppContainer broker
- animation pacing from display timing rather than a fixed timer ceiling
- signed releases + MSIX/winget packaging

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/DESIGN.md`](docs/DESIGN.md), and [`docs/ROADMAP.md`](docs/ROADMAP.md).

## License

Isle is licensed under GPL-3.0. The bundled Inter variable font remains licensed under the SIL Open Font License 1.1; see [`assets/fonts/OFL.txt`](assets/fonts/OFL.txt). The provider marks in `assets/icons` are trademarks of their respective owners, bundled only to identify the service each usage meter reports on.
