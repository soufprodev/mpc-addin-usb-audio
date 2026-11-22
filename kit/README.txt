================================================================================
  MPC MULTITRACK KIT
  Record every MPC track on its own channel, over one USB cable, in standalone mode
================================================================================

WHAT THIS DOES

  Your MPC shows up on your computer as a USB audio interface with 27 channels,
  while it keeps running on its own (standalone, NOT controller mode):

    USB channels 1-2  ......  the MPC's main mix (what comes out of its jacks)
    USB channels 3-27 ......  any tracks, pads or buses you choose

  You put a small effect called "USB Out" on each track you want separate, pick a
  channel number, and record those channels in your DAW (Ableton, FL Studio, Logic,
  Pro Tools, Reaper...). Every channel stays perfectly in time with the others.


WHICH MPCs IT WORKS ON

  WORKS:          MPC One, MPC Key 37
  DOES NOT WORK:  MPC Force, MPC Live II, MPC X  (their USB port is wired differently)
                  MPC Live III, MPC XL  (they run 64-bit software; the installer will
                  tell you and stop)
  NOT TESTED:     MPC One+, MPC Key 61, MPC X SE, MPC Live (1st generation).
                  Try it: if your computer never sees "MPC USB Audio", it won't work
                  on that model. Remove it with the steps at the end.


WHAT YOU NEED

  * MPC software (MPC OS) version 3.10.1 or newer.
    Check: Menu > Preferences > Info. Update the MPC first if it is older.
  * An SD card or USB stick that works in your MPC.
  * A computer and a USB cable to connect the MPC to it.
  * About 15 minutes.


WHAT'S IN THIS FOLDER

  README.txt      this file
  LICENSES.txt    credits and licenses
  multitrack\     the folder that goes on your SD card or USB stick:
      install.sh                           the installer
      uninstall.sh                         removes everything again
      MPC-USB-audio-addin-...zip           the USB audio part (leave it zipped)
      MPC USB Audio - VST - USB Out\       the USB Out plugin


================================================================================
  PART 1: COPY THE FILES (on your computer)
================================================================================

  1. Put your SD card or USB stick in your computer.
  2. Copy the whole "multitrack" folder to the TOP of the card or stick.
     Not inside another folder. It should look like:   E:\multitrack\install.sh
     Keep the folder name "multitrack" exactly as it is.
  3. Safely eject the card or stick and put it in your MPC.


================================================================================
  PART 2: TURN ON PLUGINS (on the MPC, only needed once)
================================================================================

  1. Menu > Preferences > Enhancements (Beta)
  2. Turn ON "Enable Enhancements".
  3. Turn ON "External Plugin Support".


================================================================================
  PART 3: TURN ON DEVELOPER MODE (on the MPC)
================================================================================

  Developer Mode is Akai's built-in way to run a setup command on the MPC.

  1. Menu > Preferences > Info
  2. Tap the VERSION number 3 times,
     then the SERIAL NUMBER 2 times,
     then the FIRMWARE VERSION 1 time.
  3. Open the Developer screen: hold SHIFT and press MENU, then tap "Developer".
  4. Open the SHELL (the terminal). You'll see a welcome message and a line
     ending in   root@...#   That's where you type.


================================================================================
  PART 4: INSTALL (on the MPC)
================================================================================

  >>> SAVE YOUR PROJECT FIRST. The MPC software restarts at the end. <<<

  1. Tap KEYBOARD at the bottom of the screen to bring up the keyboard
     (or plug a USB computer keyboard into the MPC).

  2. Type this line exactly, with the spaces, then press Enter:

         sh /media/*/multitrack/install.sh

     Tips:  "/" is the forward slash (next to the right Shift key on a keyboard).
            "*" is Shift + 8.
            There is a space after "sh" and nowhere else.

  3. It shows "1 of 3", "2 of 3", "3 of 3". When it asks

         Continue? [y/N]

     press   y   and then Enter.

  4. The MPC software restarts by itself. Wait until it is back.

  5. Turn Developer Mode OFF again when you are done. While it is on, anyone on
     your Wi-Fi can connect to the MPC without a password.

  Done. You never need to do Parts 3 and 4 again, except after an MPC software
  update (see "After an MPC update" below).


================================================================================
  PART 5: USE IT (on the MPC)
================================================================================

  Add "USB Out" as the LAST insert effect on each track you want on its own
  channel. You'll find it in the effect list under External Plugins.

  Open it. It has 3 controls:

    USB CH   Which channel it goes to on your computer. Give every track its own.
             Stereo uses two channels, so count up by 2:

               1st track  ->  3   (shows "3-4")
               2nd track  ->  5   (shows "5-6")
               3rd track  ->  7   (shows "7-8")
               ...and so on, up to 25 ("25-26")

    MODE     Stereo (normal), or Mono: puts the track on ONE channel. Good for kick,
             bass or vocals, and fits more tracks (up to 25).

    THROUGH  On (normal): you still hear the track on the MPC as usual.
             Off: the track goes ONLY to the computer, not to the MPC's own outputs.

  DON'T put USB Out on the master. The master mix is already on channels 1-2.

  DRUM KITS: you can put USB Out on single PADS (pad inserts) to get the kick,
  snare, hats... each on their own channel.

  Good to know:
  * USB Out records the track before its volume fader, so you get clean stems
    at full level. Moving the MPC's faders doesn't change what is recorded.
  * Two USB Outs set to the same channel are mixed together on that channel.
  * The channel numbers are saved inside your MPC project.

  WHAT THE SCREEN SAYS NEXT TO USB CH:
    "3-4"       working
    "NoAddin"   the USB part isn't installed or running: do Parts 3 and 4
    "3-4 !"     the USB part is running but that channel is switched off:
                run the installer again (Parts 3 and 4); it fixes the settings


================================================================================
  PART 6: SET UP YOUR COMPUTER
================================================================================

  Connect the MPC's USB port to your computer. If your computer was already
  connected before installing, UNPLUG AND REPLUG the USB cable once.

  IMPORTANT: set your DAW to 44100 Hz (44.1 kHz). That's the MPC's sample rate.

  MAC
    It works straight away: no driver needed. In your DAW, choose
    "MPC USB Audio" as the input device and turn on its inputs.

  WINDOWS
    Windows sees it right away as "MPC USB Audio". Most DAWs on Windows want an
    ASIO driver. The free ASIO4ALL driver works:

    1. Download and install ASIO4ALL from  www.asio4all.org
       (close your DAW first, open it again afterwards).
    2. In your DAW's audio settings:
         Driver type:  ASIO
         Device:       ASIO4ALL v2
         Sample rate:  44100
    3. Open ASIO4ALL's own settings (in Ableton: "Hardware Setup"; in FL Studio:
       "Show ASIO panel"). Click the small wrench icon for the full list, then:
         ON:   the MPC's INPUT
         ON:   the OUTPUT you listen on (your normal audio interface or headphones)
         OFF:  everything else, including the MPC's OUTPUT
       (Leaving the MPC's output on can cause a feedback loop.)
    4. In your DAW, turn on all the MPC inputs (in Ableton: "Input Config").

  Do NOT use the "Akai Pro MPC ... ASIO" driver: that one is only for controller
  mode, and won't see this.

  IN YOUR DAW (example):
    Make one audio track per MPC channel and set its input:

      DAW track                 Input
      MPC main mix              1/2
      1st USB Out track (3)     3/4
      2nd USB Out track (5)     5/6
      ...

    Set the tracks' monitoring to Off (you already hear the MPC), arm them,
    press record in the DAW, then press play on the MPC.

  If you hear an occasional tiny click on long recordings, listen through the
  MPC's own headphone or main outputs while recording, and switch your other
  interface's output off in ASIO4ALL.


================================================================================
  IF SOMETHING GOES WRONG
================================================================================

  "sh: can't open ..." or "No such file"
      The "multitrack" folder isn't at the top of the card or stick, or it was
      renamed. Check Part 1. Also check the line is typed exactly.

  The installer says STOPPED
      It tells you why on the line after STOPPED. Nothing was changed.

  USB Out says "NoAddin"
      The USB part isn't running. Do Parts 3 and 4 (again).

  USB Out says "3-4 !"
      Run the installer again (Parts 3 and 4). It fixes the settings.

  The computer doesn't show "MPC USB Audio"
      Unplug and replug the USB cable, and wait 30 seconds. Try another cable or
      USB port. Still nothing on an MPC from the "NOT TESTED" list: that model
      isn't supported.

  Windows only shows 2 or 4 channels
      Windows remembered an older version. Open Device Manager > Sound, video
      and game controllers, right-click "MPC USB Audio" > Uninstall device,
      then unplug and replug the MPC.

  You hear nothing in the DAW
      DAW sample rate must be 44100. Inputs must be turned on, the track's input
      set to the right pair, and the MPC must be playing.


================================================================================
  AFTER AN MPC UPDATE / KEEPING IT WORKING
================================================================================

  * After updating the MPC's software, open a USB Out. If it says "NoAddin",
    the update removed the USB part: just do Parts 3 and 4 again.
  * If a plugin manager on your MPC offers an update for "MPC USB audio addin",
    SKIP IT. That would install the original version, which doesn't have the
    USB Out channels. Reinstall from this kit if that happens.
  * Keep the "multitrack" folder on your card. Reinstalling takes 2 minutes.


================================================================================
  REMOVE EVERYTHING
================================================================================

  1. Save your project. Turn on Developer Mode and open the SHELL (Part 3).
  2. Type, then Enter:

         sh /media/*/multitrack/uninstall.sh

  3. Answer y if it asks. The MPC software restarts, and everything is gone.
     Projects that used USB Out still open; the plugin is just missing.


================================================================================
  PLEASE READ
================================================================================

  * This is a free community project. It is NOT made or supported by Akai or
    inMusic, and their support cannot help with it.
  * It runs inside the MPC software. Save your work often, and test it before
    using it at a show.
  * Use at your own risk (see LICENSES.txt).
