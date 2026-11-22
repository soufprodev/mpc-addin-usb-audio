# USB Out

An insert effect for standalone MPCs (MPC OS 3.10.1+, built with Akai's MPC Plugin SDK). It sends
the audio at its insert slot to one channel of the MPC USB Audio device, so a DAW can record every
track on its own channel.

| Control | Values | What it does |
|---|---|---|
| USB Ch | 3-27 | The USB channel this goes to (stereo uses this one and the next). 1-2 are the main mix. |
| Mode | Stereo / Mono | Mono sends L+R as one channel |
| Through | On / Off | Off: silent in the MPC's own mix, still sent to USB |

It needs the mpc-addin-usb-audio addin (with `send` channels in `to_host`). Without it the plugin
shows "NoAddin" and passes the audio through untouched.

Build with the SDK: copy this folder to `plugins/UsbOut` in `mpc-plugin-builder-sdk`, then run
`python tools/build.py plugins/UsbOut`. Full setup: `docs/MULTITRACK.md` in the addin repository.
