# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.1] - 2026-07-02

Stability, safety, and OBS-ecosystem compliance pass. No feature changes; the
`telestrator.*` hotkey vocabulary and source id are unchanged.

### Fixed
- **Cross-thread data race** on the projector-close request (a `std::string`
  shared between the hotkey and graphics threads) — heap-corrupting in theory,
  now gone: projector open/close runs as a queued UI task. All shared
  tool/color/style/arm state is `std::atomic` now, and command flags are
  consumed with `exchange` so no press can be lost.
- **Projector close is process-guarded:** only windows belonging to the OBS
  process can be closed, so a title collision with another app is harmless. The
  Program-projector hotkey is now a stateless toggle (closing the window with
  its X no longer desyncs it), and projector hotkeys work even with no
  telestrator source in any scene.
- **Fresh render targets are cleared** at creation and re-baked after a base
  resolution change — no more ghost ink from recycled GPU memory, and committed
  strokes survive a canvas resize.
- **Multiple telestrator sources are safe:** one instance drives the engine,
  extras mirror its ink; fades no longer run at double speed and undo/clear no
  longer desync canvases.
- Toggling laser or auto-fade mid-stroke no longer makes the stroke vanish at
  release (fade timing is captured at press).
- Studio-mode preview no longer freezes stale laser ink when disarmed.
- `sim_stroke` websocket requests are bounded (duration ≤ 30 s, ≤ 4096 points,
  queue depth ≤ 32).
- Websocket vendor requests are unregistered at module unload.

### Changed
- **All user-visible strings are localized** (`data/locale/en-US.ini`) — the
  release zip now ships the `data/` folder, which also removes the startup
  "Failed to load 'en-US' text" warning. Translations welcome.
- GitHub Actions CI restored from the current obs-plugintemplate (Windows,
  macOS, Ubuntu builds + format checks).
- `ENABLE_QT` / `ENABLE_FRONTEND_API` now default ON (a plain `cmake -B build`
  configures correctly).
- Template support files carry proper copyright; bundled GPLv2 components are
  documented in `THIRD_PARTY_NOTICES.md`.

## [1.0.0] - 2026-07-01

First release of the native C++ / libobs telestrator.

### Engine
- **Telestrator source** (`telestrator`): canvas + preview render-target
  architecture with stroke history (undo / redo by replay), temp strokes
  (laser, auto-fade), and an optional armed-indicator dot.
- **Broadcast-quality rendering:** supersampled anti-aliased ink, solid filled
  arrowheads, continuous evenly-spaced dashes (shapes dash properly around
  corners), uniform-alpha translucent ink (highlighter and fades composite as
  one layer), translucent filled cone.
- **Tools:** pen, line, arrow, double arrow, curved arrow (follows the drag
  path, bows either direction), rectangle, ellipse, cone, spotlight,
  horizontal guide line, vertical guide line, eraser.
- **Styles:** dashed, filled, highlighter, opacity control, laser (fading
  ink), auto-fade timer.

### Input
- **Telestrator Draw dock**: a live program view with native Qt input; ink
  lands pixel-exact under the cursor (HiDPI and letterbox aware). Right-click
  for Fit to Window or Fill Window. This is the canonical drawing surface: no
  projector, no global cursor polling.
- Legacy Win32 projector / main-preview cursor input kept behind an opt-in
  settings toggle.

### Control surface
- **Three native docks** (Tools / Color / Replay) in OBS's own theme: checkable
  tool palette, style toggles, brush-size slider, OBS's Select Color grid with
  an active-swatch ring, undo / redo / clear, the replay markup flow, and
  one-click **Add to Current Scene / Remove from Scene** management. Dock state
  syncs from the engine, so hotkeys and Stream Deck stay in lockstep. One-time
  canonical dock placement on first load.
- **Hotkeys:** the full `telestrator.*` vocabulary (tools, colors, sizes,
  styles, arm, undo/redo/clear, replay markup, projector management).
- **obs-websocket vendor requests:** `set_color` (arbitrary RGB ink) and
  `sim_stroke` (scripted strokes through the canonical input path, used by the
  automated demo and integration tests).

### Replay
- **Markup replay:** save the replay buffer, draw over the clip, replay it
  again, then resume live. No scene switch.

[1.0.1]: https://github.com/brendanwelsh/obs-telestrator/releases/tag/v1.0.1
[1.0.0]: https://github.com/brendanwelsh/obs-telestrator/releases/tag/v1.0.0
