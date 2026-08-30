# Roadmap

## 0.1 — shell foundation (this repository)

- [x] native DirectComposition surface
- [x] premultiplied transparent swap chain
- [x] spring geometry
- [x] system metrics
- [x] GSMTC media metadata
- [x] show/hide tray + hotkey
- [x] fullscreen policy
- [x] settings view
- [x] out-of-process plugin v1
- [x] x64/ARM64 presets

## 0.2 — interaction quality

- [ ] DirectComposition/display-timed frame pacing
- [ ] superellipse/squircle path instead of rounded-rect silhouette
- [x] album artwork + previous / play-pause / next media buttons
- [ ] draggable media timeline
- [ ] hover intent model that considers pointer velocity
- [ ] monitor picker + top-left/top-right/bottom placement
- [ ] reduced-motion support from Windows accessibility settings

## 0.3 — useful daily surfaces

- [ ] OLE file shelf / quick drop
- [ ] volume and per-session mixer provider
- [ ] brightness control where hardware exposes it
- [ ] timer provider
- [ ] clipboard preview provider
- [ ] notification listener provider with explicit permission UX
- [ ] optional Bluetooth battery provider

## 0.4 — plugin hardening

- [ ] capability declarations
- [ ] per-plugin enable/disable/settings
- [ ] restart/backoff policy
- [ ] AppContainer broker profile for low-capability plugins
- [ ] signed plugin bundles with hashes, without central-store lock-in
- [ ] documented SDK helpers for Rust/C#/TypeScript/Python

## 1.0 — distribution

- [ ] automated release signing
- [ ] MSIX and portable ZIP
- [ ] winget manifest
- [ ] crash dumps opt-in, no analytics by default
- [ ] stable protocol v1 compatibility promise
