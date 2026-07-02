# Contributing

Thanks for your interest! Issues and PRs are welcome.

## Building

Windows (the primary target today):

```
cmake --preset windows-x64        # fetches libobs + Qt via buildspec.json
cmake --build --preset windows-x64
```

Drop `build_x64/rundir/RelWithDebInfo/telestrator.dll` into OBS's
`obs-plugins/64bit/` and restart OBS. The project is based on the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate), so the
macOS / Linux presets exist but aren't exercised yet. Porting help is welcome:
the input path is pure Qt, so it should be close.

## Code style

- C++17, OBS Studio conventions. Format with the repo's `.clang-format`
  (`build-aux/run-clang-format`); CMake files with `.gersemirc`
  (`build-aux/run-gersemi`).
- The engine lives in `src/plugin-main.cpp`. Hotkey handlers only set flags;
  the graphics thread (`video_tick`) does the work.

## The compat contract (please don't break it)

External controllers (Stream Deck, Ulanzi dial, the docks) all speak the same
vocabulary. Keep these stable:

- the source id `telestrator`,
- every `telestrator.*` hotkey name,
- the shared command vocabulary (`tool:pen`, `color:red`, `sizedown`, and so
  on). See [`docs/STREAMDECK-SPEC.md`](docs/STREAMDECK-SPEC.md).

Adding new commands is great; renaming or removing existing ones breaks
everyone's key mappings.

## Testing a change

The GPU render path and native input can only be verified inside OBS. For
scripted verification, `tools/verify.mjs` (scene setup, probing, screenshots)
and `tools/drive.mjs` (an all-tools demo via the `sim_stroke` obs-websocket
vendor request) drive a running OBS without any window focus games:

```
OBS_WS_PASSWORD=... node tools/verify.mjs setup
OBS_WS_PASSWORD=... node tools/drive.mjs
```

## License

MIT. Keep the Herschel / Tari / katarai lineage attribution intact in `LICENSE`,
the README, and source headers. This project exists because of the original
obs-whiteboard.
