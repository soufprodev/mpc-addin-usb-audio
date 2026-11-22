#!/bin/sh
# MPC Multitrack Kit: installer. Runs ON the MPC, in the Developer Mode shell:
#
#     sh /media/*/multitrack/install.sh
#
# 1. copies the "USB Out" plugin into "VST2 Plugins" on the drive this folder is on
# 2. unpacks the USB audio addin and runs its installer (which asks y/N and restarts MPC)
# 3. an older install keeps its own settings file: switches it to the multitrack settings

KIT=$(cd "$(dirname "$0")" && pwd)
DRIVE=$(dirname "$KIT")
PLUGIN="MPC USB Audio - VST - USB Out"
CONF=${MPCKIT_CONF:-/data/mpc-addins/usb-audio/usbaudio.conf}
TMP=${MPCKIT_TMP:-/data/mpc-multitrack-tmp}
MPC_BIN=${MPCKIT_MPC:-/usr/bin/MPC}

say() { echo; echo "== $*"; }
fail() {
  echo
  echo "STOPPED: $*"
  echo "Nothing else was changed. See README.txt, \"If something goes wrong\"."
  exit 1
}

echo
echo "MPC Multitrack Kit installer"
echo "Kit folder: $KIT"

[ "$(id -u)" = 0 ] || fail "run this in the MPC's Developer Mode shell (its prompt starts with root@)."
[ -f "$MPC_BIN" ] || fail "this does not look like a standalone MPC."

# Byte 5 of a program file says 1 = 32-bit, 2 = 64-bit. The addin is 32-bit only.
bits=$(dd if="$MPC_BIN" bs=1 skip=4 count=1 2>/dev/null | od -An -tu1 2>/dev/null | tr -d ' ')
[ "$bits" = 2 ] && fail "this MPC runs 64-bit software (MPC Live III, MPC XL or newer). The kit works on 32-bit MPCs only."

ADDIN_ZIP=$(ls "$KIT"/MPC-USB-audio-addin-*-mpc-armv7.zip 2>/dev/null | head -n 1)
[ -n "$ADDIN_ZIP" ] || fail "the file MPC-USB-audio-addin-...zip is missing from $KIT."
[ -d "$KIT/$PLUGIN" ] || fail "the folder \"$PLUGIN\" is missing from $KIT."

say "1 of 3: copying the USB Out plugin to \"VST2 Plugins\" on this drive"
mkdir -p "$DRIVE/VST2 Plugins" || fail "could not make the VST2 Plugins folder on $DRIVE."
rm -rf "$DRIVE/VST2 Plugins/$PLUGIN"
cp -r "$KIT/$PLUGIN" "$DRIVE/VST2 Plugins/" || fail "could not copy the plugin to $DRIVE/VST2 Plugins."
echo "   done: $DRIVE/VST2 Plugins/$PLUGIN"

say "2 of 3: unpacking the USB audio addin"
rm -rf "$TMP"
mkdir -p "$TMP" || fail "could not make $TMP."
unzip -o -q "$ADDIN_ZIP" -d "$TMP" || fail "could not unzip $ADDIN_ZIP."
SRC=$(ls -d "$TMP"/MPC-USB-audio-addin-* 2>/dev/null | head -n 1)
[ -f "$SRC/install.sh" ] || fail "the addin zip has no install.sh in it."
echo "   done"

# The addin's installer keeps an existing settings file, so an older install would stay on the
# basic settings (main mix only). Switch it to multitrack: later lines win.
if [ -f "$CONF" ] && ! grep -q '^to_host=.*send' "$CONF"; then
  printf '\n# MPC Multitrack Kit\nto_host=out1,out2,send1-25\nsample_bytes=3\n' >> "$CONF"
  echo "   your existing addin settings were switched to multitrack"
fi

say "3 of 3: installing the addin"
echo "   It asks \"Continue? [y/N]\": press y, then Enter."
echo "   The MPC software restarts at the end. SAVE YOUR PROJECT FIRST."
echo
cd "$SRC" && sh install.sh
