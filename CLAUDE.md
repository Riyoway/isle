# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Isle is a Windows-only, C++23 always-on-top overlay ("dynamic island"): a Win32 `HWND` rendered with
Direct2D/DirectWrite onto a DirectComposition swap chain. There is no UI framework, no XAML, no WebView.
Everything visible is drawn by `src/platform/Renderer.cpp`.

## Build, test, run

```powershell
cmake --preset windows-x64-release        # or windows-x64-debug / windows-arm64-release
cmake --build --preset release            # or debug / arm64-release
ctest --test-dir build/x64-release -C Release --output-on-failure
```

`scripts/build.ps1 -Configuration Debug -Arch x64` wraps the same presets. Output lands at
`build/x64-release/Release/Isle.exe`; the post-build step copies `assets/fonts` and `assets/icons`
next to it.
CI (`.github/workflows/build.yml`) runs exactly the three commands above on `windows-2022`.

The test suite is a single assert-based binary (`tests/core_tests.cpp`, no framework). Run it directly
(`build/x64-release/Release/IsleCoreTests.exe`) instead of via CTest when iterating. Note the
`IsleCoreTests` target compiles only `tests/core_tests.cpp` + `src/core/ActivityStore.cpp`, so only
header-only core logic (`Spring.h` helpers) and `ActivityStore` are testable — put new pure logic in
`src/core/*.h` if it needs coverage.

Isle enforces a single instance via a named mutex; kill the running `Isle.exe` before launching a rebuild.

### CMake gotchas

- Sources are listed explicitly in `CMakeLists.txt`. A new `.cpp` must be added there by hand.
- The application manifest is fed to the linker with `/MANIFESTINPUT`. Do **not** also embed
  `app.manifest` as `RT_MANIFEST` in `resources.rc` — two resource-ID-1 manifests fail with CVT1100.

## Architecture

### Data flow

```
providers (background/event-driven) → ActivityStore (mutex) → snapshot() → RenderState + Activity[] → Renderer
```

`OverlayWindow` (`src/platform/OverlayWindow.cpp`) is the only orchestrator: it owns the HWND, the
springs, `Settings`, `ActivityStore`, every `IProvider`, the tray icon, and the `Renderer`. Each timer
tick it calls `provider->tick()`, steps the springs, then hands the renderer an immutable snapshot.

Hard rule from `docs/ARCHITECTURE.md` and `CONTRIBUTING.md`: **providers never draw and the renderer
never calls OS or plugin code.** A provider's only output is `Activity` values pushed into the store.

### Providers

All implement `IProvider` (`start/stop/tick/invoke`) and are registered in `OverlayWindow::on_create()`:

| Provider | Source ids | Mechanism |
| --- | --- | --- |
| `SystemProvider` | `system` | `GetSystemTimes`, `GlobalMemoryStatusEx`, `GetSystemPowerStatus`, PDH for GPU |
| `AIUsageProvider` | `ai.<provider>` | reads Codex local sessions and refreshes selected providers directly over WinHTTP on a `std::jthread`; credentials come from provider environment variables or the Windows Credential Manager |
| `MediaProvider` | `media` | GSMTC via C++/WinRT, event-driven |
| `ShortcutProvider` | `shortcut.app`, `shortcut.command` | `ShellExecuteW`; re-publishes when `settings.ini` mtime changes |
| `PluginHost` | `<plugin-id>` | out-of-process children over NDJSON stdio |

`Activity.priority` drives sort order in `ActivityStore::snapshot()`. `remove_source()` is the idiom for
a provider replacing its whole set of activities.

### Rendering and geometry

`RenderState` (`src/core/Types.h`) is the complete per-frame input: spring values, DPI scale, flattened
settings, and hit-test-relevant flags. `OverlayWindow::apply_settings_to_render_state()` mirrors
`Settings` into it — a new user-visible setting must be added to both structs.

Layout lives **only** in the renderer. `Renderer::island_rect()`, `widget_rect()` and `expanded_height()`
are public because `OverlayWindow` hit-tests against them; never re-derive rectangles in the window code.
Widget cards are laid out by `widgetOrder` in `widget_rect()`, with `full_width_widget()` /
`widget_height()` (file-local in `Renderer.cpp`) deciding the half/full-width flow.

The HWND is created at a fixed maximum size and a window region is clipped to the current island bounds,
so expanding never recreates the composition target — but it means the transparent remainder would swallow
clicks if `update_region()` were skipped.

Motion is springs, not easings (`src/core/Spring.h`). Each animated quantity has its own `Spring` with
stiffness/damping configured in the `OverlayWindow` constructor. Content cross-fades follow the silhouette;
see `docs/DESIGN.md` for the staging order.

### Interaction

Mouse hits resolve to an integer control id via `OverlayWindow::control_at()`. Ids are allocated in
banded ranges (`kControlGear`, `kControlMediaBase`, `kControlSettingBase`, `kControlWidgetBase`,
`kControlShortcutBase`, `kControlAppearanceBase`, `kControlAiBase`); a new interactive element needs a
hit-test function, a band slot, and a branch in `on_left_button_up`.

The window is `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP` and must
stay non-activating — it cannot host text entry. Settings needing typed input belong in a separate
activated window.

### Settings

`Settings::load()`/`save()` use Win32 INI APIs against `%LOCALAPPDATA%\Isle\settings.ini`. Parsing is
field-by-field with clamped fallbacks so a corrupt file degrades instead of failing. Several providers
call `Settings::load()` themselves rather than receiving a reference.

### AI providers table

`src/core/AIProviders.h` holds a `constexpr std::array<AIProviderInfo, 56>`. Adding an entry means bumping
that array size; `Settings::aiColors` / `aiRings` are sized from `kAIProviders.size()` and their INI
sections are named `ai.<id>`.

`AIUsageProvider` does not depend on another application. Built-in adapters call provider endpoints
directly (Claude, Gemini, Cursor, Copilot, OpenRouter, OpenAI, DeepSeek, z.ai, ElevenLabs, Poe,
Venice, DeepInfra, Cline, Amp, Augment, Crof, OpenCode Go and local Ollama). Other entries accept
`ISLE_<PROVIDER>_USAGE_URL` with `ISLE_<PROVIDER>_API_KEY`, `ISLE_<PROVIDER>_TOKEN`, or
`ISLE_<PROVIDER>_COOKIE`. Tokens can also be stored as generic credentials under `Isle/<provider>`.

### Plugins

Plugins are separate processes under `%LOCALAPPDATA%\Isle\plugins\<name>\plugin.json`, launched with
redirected stdio and `--isle-plugin`, speaking NDJSON both ways (`plugin-sdk/protocol.md`). The host
namespaces incoming ids as `<plugin-id>:<activity-id>`. Plugins describe activities; they never draw.
`scripts/install-example-plugin.ps1` installs the bundled example. This is a stability boundary, not a
sandbox — do not describe it as one.

### Threading

Main STA thread owns windowing, animation, and all D2D/D3D objects. GSMTC callbacks, the direct AI refresh
`jthread`, and one reader thread per plugin only touch `ActivityStore` (mutex-protected). No provider or
plugin may hold a Direct2D/Direct3D object.

## Conventions

- `/W4 /permissive- /utf-8`; wide strings and `W` Win32 APIs throughout; `Microsoft::WRL::ComPtr` for COM.
- Prefer event-driven Windows APIs over polling; no WMI, no injected hooks, no WebView in the overlay.
- Failures degrade rather than throw: WARP fallback for D3D, per-field settings fallback, malformed plugin
  lines ignored. Only `wWinMain` catches.
- `// ponytail:` comments mark deliberate simplifications and name their upgrade path — keep the style
  when leaving one.
