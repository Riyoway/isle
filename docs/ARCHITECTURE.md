# Architecture

## Rendering path

```text
Win32 HWND
  └─ DirectComposition target
      └─ DirectComposition visual
          └─ DXGI composition swap chain (BGRA8, premultiplied alpha)
              └─ Direct2D device context
                  └─ Isle vector UI + DirectWrite text
```

The HWND uses `WS_POPUP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP`. The swap chain is created with `CreateSwapChainForComposition` and `DXGI_ALPHA_MODE_PREMULTIPLIED`, so transparent pixels are composed with the desktop rather than copied into a traditional opaque HWND backbuffer.

The actual HWND is larger than the collapsed island so expansion does not recreate the composition target. A window region is updated to the current island bounds, preventing the transparent remainder of the fixed render surface from becoming a giant invisible click blocker.

## State model

`OverlayWindow` is the orchestration boundary. It owns:

- geometry springs;
- fullscreen/manual visibility state;
- monitor placement and DPI scale;
- `ActivityStore`;
- built-in providers;
- the external `PluginHost`;
- the renderer.

Providers publish immutable-ish `Activity` values into `ActivityStore`. The renderer receives a sorted snapshot each frame. This deliberately prevents the renderer from calling arbitrary OS/plugin code while drawing.

## Animation

Geometry is integrated with a damped spring:

```text
acceleration = (target - value) * stiffness - velocity * damping
velocity += acceleration * dt
value += velocity * dt
```

The integration delta is clamped after debugger stops and sleep/resume. Width, height, hover amount, expansion amount and visibility each have independent springs. Rendering currently runs from an 8 ms timer ceiling while DXGI `Present(1)` synchronizes delivery to DWM.

The next pacing iteration should use display timing / frame-latency waitable objects so the app does no extra animation ticks on a 60 Hz screen and naturally follows high-refresh monitors.

## Built-in providers

### SystemProvider

Uses cheap public Win32 calls:

- CPU: `GetSystemTimes` delta sampling
- RAM: `GlobalMemoryStatusEx`
- battery/AC: `GetSystemPowerStatus`

No WMI process and no permanent performance counter session is required.

### AIUsageProvider

AI usage is fetched by Isle itself with WinHTTP. The provider has direct adapters
for the common API, OAuth, cookie, and local-session sources; providers without a
stable built-in endpoint can be configured with `ISLE_<PROVIDER>_USAGE_URL`.
Credentials are read from provider environment variables or the Windows
Credential Manager and are not written to the settings file.

### MediaProvider

Uses Windows GSMTC (`GlobalSystemMediaTransportControlsSessionManager`) through C++/WinRT. It is event-driven and publishes a now-playing activity when the current session or playback metadata changes.

## Plugin boundary

External plugins are intentionally out-of-process. A plugin lives in:

```text
%LOCALAPPDATA%\Isle\plugins\my-plugin\
  plugin.json
  ... executable / scripts ...
```

Isle launches it with redirected stdio. Both directions are NDJSON. The v1 renderer only accepts declarative activity data rather than arbitrary plugin-drawn pixels.

This is a stability boundary, **not yet a sandbox boundary**. A v1 plugin still has the permissions of the current user. Future capability-scoped plugins can be brokered through AppContainer without breaking the visual protocol.

## Threading

- UI/rendering/window state: main STA thread
- GSMTC async work: C++/WinRT callbacks/coroutines, store updates are mutex-protected
- each external plugin: one blocking reader thread
- renderer: main thread only

No plugin or provider holds a D2D/D3D object.

## Error containment

- malformed plugin lines do not terminate the host;
- plugin exit removes plugin activities and surfaces a disconnected status;
- Direct2D target loss recreates swap-chain size resources;
- D3D hardware-device creation falls back to WARP;
- a bad settings file falls back field-by-field to defaults.
