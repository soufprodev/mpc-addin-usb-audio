## MPC Multitrack Kit

Record every MPC track on its own channel into your DAW, over one USB cable, with the MPC in **standalone mode**.

- USB channels **1-2**: the MPC's main mix
- USB channels **3-27**: any tracks, pads or buses you choose, using the **USB Out** plugin

### Download

**[MPC-Multitrack-Kit-1.3.1.zip](https://github.com/soufprodev/mpc-addin-usb-audio/releases/download/multitrack-v1.3.1/MPC-Multitrack-Kit-1.3.1.zip)** (also under *Assets* below). Unzip it and open **README.txt**: it walks you through everything, no coding needed.

### In short

1. Copy the `multitrack` folder to the top of your MPC's SD card or USB stick.
2. On the MPC: turn on External Plugin Support and Developer Mode.
3. In the Developer shell, type `sh /media/*/multitrack/install.sh` and answer `y`.
4. Put **USB Out** on your tracks, give each its own channel, record in your DAW at 44.1 kHz.

### Which MPCs

| | |
|---|---|
| **Works** | MPC One, MPC Key 37 |
| **Doesn't work** | Force, MPC Live II, MPC X (USB port wiring); MPC Live III, MPC XL (64-bit) |
| **Not tested** | MPC One+, Key 61, X SE, Live (1st gen) |

Needs **MPC OS 3.10.1 or newer**. Windows: use the free ASIO4ALL driver (see README). Mac: works without a driver.

### Credits

Built on [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio) by Jacob Sabella, the Open MPC installer, and Akai's MPC Plugin Builder SDK. MIT licensed. Not made or supported by Akai / inMusic: use at your own risk and save your work often.
