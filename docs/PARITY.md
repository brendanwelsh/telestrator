# Parity with the Lua engine

This C++ engine is a faithful port of `obs-telestrator-lua` — a **superset**: every
Lua feature is present, plus broadcast extras. The drawing math (Catmull-Rom
freehand, arrow/shape geometry, laser/auto-fade temp strokes, the armed indicator)
mirrors the Lua engine 1:1.

## Lua → C++ (all present)
| Lua | C++ |
|---|---|
| Tools: pen, line, arrow, rect, ellipse | ✅ same |
| Colors: yellow, red, green, blue, white, custom | ✅ same |
| toggle / armon / armoff (arm drawing) | ✅ `telestrator.toggle` / `.armon` / `.armoff` |
| toolcycle, colorswap, sizetoggle, sizedown | ✅ same |
| erasertoggle, laser | ✅ same |
| undo, redo, clear | ✅ same |
| closeprojectors | ✅ (Win32 projector close) |
| size (1..max) | ✅ size presets + up/down |
| custom_color | ✅ + a color **wheel** |
| **autofade** (fade all ink after N s) | ✅ `telestrator.autofade` (cycle off/5/10s) |
| **show_indicator** (armed dot) | ✅ `telestrator.indicator` (toggle) |
| projector keyword / debug log (script settings) | n/a — fixed projector title match |

## C++ extras (beyond the Lua)
- **Tools:** double-arrow, curved "magic" arrow, **cone**, **spotlight**, **horizontal line** (first-down), **vertical line**.
- **Rendering:** supersampled anti-aliasing, solid filled arrowheads, cap-aware continuous dashes, uniform-alpha translucent ink (highlighter/fades).
- **Styles:** dashed, filled, highlighter, **adjustable opacity**.
- **Colors:** orange, cyan, + the color wheel.
- **Replay markup** with play / pause / stop / restart transport.
- **Native operator dock** (grouped, labeled), **embedded icons**.
- **Stream Deck** + **Ulanzi dial** companion plugins (shared command vocabulary).
- Native C++/libobs performance; no LuaJIT dependency.

The last two Lua-only settings (auto-fade, armed indicator) were wired into the
dock + hotkeys (this commit), so there are no remaining feature gaps.
