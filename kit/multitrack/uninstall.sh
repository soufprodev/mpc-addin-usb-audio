#!/bin/sh
# MPC Multitrack Kit: removes everything the kit installed. Runs ON the MPC, in the Developer Mode shell:
#
#     sh /media/*/multitrack/uninstall.sh

PLUGIN="MPC USB Audio - VST - USB Out"

echo
echo "MPC Multitrack Kit: removing"

for d in /media/*/"VST2 Plugins/$PLUGIN"; do
  [ -d "$d" ] && rm -rf "$d" && echo "   removed the plugin: $d"
done
rm -rf /data/mpc-multitrack-tmp

if [ -f /data/mpc-addins/usb-audio/uninstall.sh ]; then
  echo "   removing the USB audio addin (the MPC software restarts; save your project first)"
  sh /data/mpc-addins/usb-audio/uninstall.sh
else
  echo "   the USB audio addin was not installed"
fi
