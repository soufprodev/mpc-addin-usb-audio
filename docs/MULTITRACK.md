# Multitrack recording from a standalone MPC into a DAW

Record your MPC tracks into a computer over one USB cable, each track on its own channel, with the
MPC in standalone mode (MPC OS 3.10.1 or later).

## The two pieces

| Piece | What it is | Where it goes |
|---|---|---|
| **The addin** (`libmpc_usb_audio.so`) | Makes the MPC show up on the computer as a USB audio interface, "MPC USB Audio", with 27 channels. Channels 1-2 are always the main mix. | Installed once, over SSH (Developer Mode) |
| **USB Out** (a plugin, built with Akai's official MPC Plugin SDK) | An insert effect. Whatever track, pad or bus you put it on gets copied to the USB channel you choose (3 to 27). The sound in the MPC doesn't change. | The `VST2 Plugins` folder on a drive |

The two talk to each other inside the MPC. USB Out only hands audio over, and the addin does the USB
part, so every channel stays sample-aligned with the main mix.

## Channel layout on the computer

| USB channels | What's on them |
|---|---|
| 1-2 | The MPC's main mix (what comes out of its jacks) |
| 3-27 | Whatever USB Out instances send there: 12 stereo tracks (3-4, 5-6 ... 25-26) plus one mono (27), or up to 25 mono tracks, or any mix |

27 is the most the MPC's Linux USB audio driver allows in one direction.

## Setting up the MPC (once)

1. Update to MPC OS 3.10.1 or later.
2. **Menu > Preferences > Enhancements**: turn on *Enable Enhancements*, then *External Plugin
   Support*.
3. Get the builds (see "Getting the builds" below).
4. **USB Out plugin**: unzip `usb-out-...-mpc.zip` into the `VST2 Plugins` folder on the MPC's
   internal drive (or on a USB stick or SD card you leave in).
5. **The addin**: copy `MPC-USB-audio-addin-...-mpc-armv7.zip` to a USB stick or SD card. On the
   MPC, hold **SHIFT + MENU** for Developer Mode, choose **Developer**, and open the terminal.
   Opening it also turns on SSH, so you can type the same commands from your computer if you prefer
   a keyboard. Then:

   First find your stick with `ls /media` (if it isn't there, try `mount` to see where drives are).

   ```
   cd /media/<your drive name>
   unzip MPC-USB-audio-addin-*-mpc-armv7.zip -d usb-audio
   cd usb-audio
   sh install.sh
   ```

   The installer asks before it changes anything, and restarts MPC at the end. It comes with the
   multitrack settings (`usbaudio.conf`) already in place. To remove it later:
   `sh /data/mpc-addins/usb-audio/uninstall.sh`.

   Turn Developer Mode off when you're done. Akai notes that its SSH server is a risk on untrusted
   networks.

6. Connect the MPC's USB-B port to the computer.

## Recording

1. On each track you want separately, add **USB Out** as the **last** insert effect and set
   **USB Ch**: 3 for the first stereo track (shows "3-4"), 5 for the next, and so on.
   - **Drum program:** put USB Out on individual **pads** (pad inserts) to get the kick, snare and
     so on on their own channels.
   - **Mode: Mono** sends the track as one channel. Use it for mono sources like a kick or bass,
     and fit up to 25 of them.
   - **Through: Off** keeps the track out of the MPC's own mix but still sends it to USB.
2. In the DAW, choose **MPC USB Audio** as the input device. Make one track per USB channel
   (input 1-2 = main mix, 3-4 = the first USB Out, ...), arm them, and press record in the DAW, then
   play on the MPC.

What the screen shows next to USB Ch:
- `3-4`: working.
- `NoAddin`: the addin isn't installed or isn't running. The plugin passes audio through and does
  nothing else.
- `3-4 !`: the addin is running, but this channel isn't one it sends (check `to_host` in
  `usbaudio.conf`).

Good to know:
- USB Out hears the track **at the point where it sits in the insert chain**. As the last insert,
  you get the track with its effects but **before** the track's volume fader and pan: clean stems
  at full level. Mixer moves on the MPC don't change the recording.
- Two USB Outs set to the same channel are added together, like a submix.
- All channels are sample-aligned with each other and with the main mix. They arrive in the DAW
  about 10 ms late, so if your DAW records against its own playback, nudge the recorded clips or set
  a recording offset.

## Getting the builds

Both pieces have to be built for the MPC's ARM processor. The easiest way needs no tools on your
computer:

1. On GitHub, fork `jacob-sabella/mpc-addin-usb-audio` and upload this version of the code to your
   fork.
2. Open the fork's **Actions** tab, choose **Multitrack build**, then **Run workflow**.
3. When it finishes (a few minutes), download **multitrack-build** at the bottom of the run. It
   holds both zips and this guide.

The workflow runs the offline tests, builds the addin, and builds USB Out with Akai's SDK (which also
runs it through Akai's MPC host simulator).

## Checking it's working

Look at `usbaudio.log` in `/data/mpc-addins/usb-audio/` (Developer terminal: `cat` it). Every
10 seconds there is a line like:

```
USB Out plugins sending on: 1,2,3,4
```

Those are send numbers. Send 1 is USB channel 3, so `1,2,3,4` means USB channels 3-6 are getting
audio. `none` means no USB Out instance is running. Check the plugin is on a track and the track is
playing.

## Troubleshooting

- **No "MPC USB Audio" on the computer:** read `usbaudio.log`. On an MPC One, MPC may build no USB
  gadget of its own in standalone mode. The addin then builds its own after 30 s (the log says
  "built our own"). In that case the computer sees the audio device but not the MPC's USB MIDI port.
- **Windows:** the built-in USB audio driver works with WASAPI. DAWs that need ASIO need a wrapper
  such as FlexASIO or ASIO4ALL. If Windows remembers the MPC with fewer channels, remove it once in
  Device Manager and plug it back in.
- **Crackles on the computer only** (not on the MPC): raise `target_frames` in `usbaudio.conf`
  (for example to 512) and restart MPC.
- **Installer says permission denied:** Developer Mode's shell runs with the MPC's own rights. If
  those aren't enough to install, the log and the installer's message say so.

## The other approach: a wide codec (`wide_out`)

There is also an experimental setting that tells MPC its audio hardware has more outputs than it
does (`wide_out=32`), so MPC's own Out 3/4, 5/6 ... could carry tracks. Whether MPC on an MPC One
offers those outputs is up to MPC. USB Out doesn't depend on it, so start with USB Out.
`usbaudio.conf.example` describes `wide_out` and `wide_in`.
