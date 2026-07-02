# Stream Deck + Ulanzi dial spec — telestrator (C++)

Spec for aligning the existing **Stream Deck plugin** to the C++ telestrator and
for a **new Ulanzi-dial plugin**. Both are *external tooling* — they drive the
engine purely over **obs-websocket** (fire `telestrator.*` hotkeys; drive the
`Telestrator Replay` media source for transport/scrub). No core-engine changes
are required for any of this.

## Command vocabulary (current C++ engine hotkeys)
Fire via obs-websocket `TriggerHotkeyByName { hotkeyName }`. Colon-form names in
the dock/deck map 1:1 to these `telestrator.*` hotkeys.

| Group | Hotkeys |
|---|---|
| Arm | `telestrator.toggle`, `.armon`, `.armoff` |
| Tools | `.tool.pen`, `.tool.line`, `.tool.arrow`, `.tool.rect`, `.tool.ellipse`, `.tool.dblarrow`, `.tool.curvedarrow`, `.tool.cone`, `.tool.spotlight`, `.tool.firstdown`, `.toolcycle` |
| Colors | `.color.yellow`, `.color.red`, `.color.green`, `.color.blue`, `.color.white`, `.color.orange`, `.color.cyan`, `.color.custom`, `.colorswap` |
| Size | `.sizetoggle` (up), `.sizedown`, `.size.thin`, `.size.med`, `.size.thick` |
| Style | `.dash`, `.fill`, `.highlight`, `.opacity` (cycle 100/66/33%), `.laser`, `.erasertoggle` |
| Options | `.indicator` (armed dot), `.autofade` (off/5s/10s) |
| Edit | `.undo`, `.redo`, `.clear` |
| Projector | `.closeprojectors`, `.closeprojector.program/preview/multiview` |
| Replay | `.replay` (save buffer + draw-on-it overlay), `.replayhide` (resume live) |

Every name above is an exact registered `telestrator.*` hotkey — fire any via
`TriggerHotkeyByName`. The dock, Stream Deck plugin, and Ulanzi plugin all map to
this one list.

## Replay transport / scrub (obs-websocket, no engine hotkey)
The replay overlay loads into a media source named **`Telestrator Replay`**.
Control it directly over obs-websocket:
- Play / Pause / Stop / Restart:
  `TriggerMediaInputAction { inputName: "Telestrator Replay", mediaAction: "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_{PLAY|PAUSE|STOP|RESTART}" }`
- **Scrub** (dial): `SetMediaInputCursor { inputName: "Telestrator Replay", mediaCursor: <ms> }`
- Replay buffer itself: `StartReplayBuffer` / `StopReplayBuffer` / `SaveReplayBuffer`.

## Zoom (dial-driven, obs-websocket — no engine change)
Magnifying the scene happens at the OBS level, not inside the telestrator source
(a source can't sample the composited scene below it). Set up a **"Zoom"** scene
item — a duplicate of the scene or a Source Mirror — and drive its transform from
the Ulanzi dial:
- **Rotate = zoom level:** `SetSceneItemTransform { sceneName, sceneItemId,
  sceneItemTransform: { scaleX, scaleY, positionX, positionY } }` — scale up and
  recenter on the region of interest.
- **Press = reset** to scale 1.0, position 0,0.
- Then draw on top with the telestrator as usual.

Keeps zoom as pure tooling; the engine is unchanged. (A future "draw a box to
zoom" flow would set the crop/scale from a drawn rectangle.)

## Stream Deck plugin — buttons to add
Keep the existing per-key `TriggerHotkeyByName` pattern (`streamdeck/.../plugin.js`,
`obs-ws.js`). Suggested new keys: Double-Arrow, Dashed toggle, Thin/Med/Thick,
Replay+Markup, Back-to-Live, and replay transport (Play/Pause/Restart) using the
media-action requests above.

## Ulanzi dial plugin (new) — `com.ulanzi.ulanzistudio.telestrator`
Mirror Brendan's existing Ulanzi plugins (`hardware/ulanzi-camera-switcher`,
`hardware/ulanzi-window-control`). Drop the folder in
`%APPDATA%\Ulanzi\UlanziDeck\Plugins\` and restart Ulanzi Studio.

```
com.ulanzi.telestrator.ulanziPlugin/
├── manifest.json        # UUID com.ulanzi.ulanzistudio.telestrator; Encoder + Keypad actions
├── plugin/app.js        # Node entry: UlanziApi + a Node obs-websocket client (port ws.js)
├── property-inspector/  # host/port/password config (reuse the dock's fields)
├── ulanzi-api/  libs/   # vendored Ulanzi SDK
└── package.json         # dep: "ws"
```

**Dial mapping (`$UD.onDialRotate` → `message.rotateEvent` 'left'|'right'):**
- Default mode: rotate = size (`telestrator.sizetoggle` / `.sizedown`), press = `telestrator.toggle` (arm).
- **Mode swap** (a Keypad key or long-press): cycle the dial's function — Size ▸ Tool (`.toolcycle`) ▸ Color (`.colorswap`) ▸ Undo/Redo (`.undo`/`.redo`) ▸ **Replay scrub**.
- **Replay scrub mode**: accumulate rotation deltas → `SetMediaInputCursor` on `Telestrator Replay` (e.g. ±250 ms per notch); press = Play/Pause.
- 7 keys = tool/color presets + Replay+Markup + Back-to-Live + transport.

**OBS-websocket client:** port the ~150-line `obs-ws.js` (Hello/Identify + SHA-256
auth + request) to Node's `ws` (see `tools/obs.mjs` in this repo for a working
Node v5 client). Host/port/password from the property inspector.

## Profiles
`.ulanziDeckProfile` / `.sdProfile` button layouts live in Brendan's
`streamdeck-design` repo — propose buttons there, build the layout in the
Ulanzi/Stream Deck UI (they're generated, not hand-written JSON).
