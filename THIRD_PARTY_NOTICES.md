# Third-party notices

obs-telestrator is MIT licensed (see [`LICENSE`](LICENSE)), but the compiled
plugin also contains the following GPLv2-licensed components. The combined
binary is therefore distributed under terms compatible with GPLv2.

## obs-plugintemplate support code

`src/plugin-support.h`, `src/plugin-support.c.in`, and the `cmake/`,
`build-aux/`, and `.github/` scaffolding come from the
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
(GNU General Public License v2.0 or later).

## obs-websocket vendor API header

`src/obs-websocket-api.h` is the vendor-API header from
[obs-websocket](https://github.com/obsproject/obs-websocket),
Copyright (C) 2020-2021 Kyle Manning, licensed under the
GNU General Public License v2.0 or later. Vendoring this header is the
documented integration path for plugins that expose obs-websocket vendor
requests.

## OBS Studio (libobs, obs-frontend-api)

The plugin links against [OBS Studio](https://github.com/obsproject/obs-studio)
libraries (GNU General Public License v2.0 or later). It is a plugin for OBS
Studio and does not redistribute OBS itself.
