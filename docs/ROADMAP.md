# Roadmap

## Done
- **First-down marker** — full-width horizontal line at the drawn height
  (`telestrator.tool.firstdown`, dock "1st Down"). The football yellow line.

## Hardware controllers
- **Stream Deck** — supported today (see [`CONTROLLERS.md`](CONTROLLERS.md)).
- **Ulanzi-dial plugin (planned)** — a dedicated Ulanzi Studio plugin: rotate for
  brush size or replay scrub, press to arm, keys for tools and colors, and the
  dial-driven zoom below. Specced in
  [`STREAMDECK-SPEC.md`](STREAMDECK-SPEC.md); not built yet.

## Sport-specific (candidates, not yet built)
- Line-of-scrimmage (blue) + first-down (yellow) as a pair.
- Offside line (soccer) — like first-down but angle/perspective-adjustable.
- Yard-line markers + down & distance **text** (needs a text layer — future).
- Player number / name labels (text layer).
- Possession / direction indicators.

## Zoom (Brendan's idea) — the clean path is tooling, not a core magnifier
A source plugin can't easily sample the composited scene *below* it, so a true
in-source magnifier is hard. Instead:
- Use an OBS **"zoom source"**: a duplicate/projector of the scene, cropped to a
  region and scaled up (a box).
- The **Ulanzi dial drives it over obs-websocket** (`SetSceneItemTransform` +
  crop): rotate = zoom level, press = reset, then you draw on top with the
  telestrator. "Draw a box with the zoom tool, then draw" = the box sets the
  zoom source's crop/scale, telestrator annotates over it.
- Pure tooling + a transform — spec it into the Ulanzi plugin (no engine change).
- Alternative (core, harder): a magnifier as an OBS **filter/effect** that
  samples the program texture. Deferred — bigger lift than a source.
