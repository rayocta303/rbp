# Tutorial — Run XDJ-RX3 rekordbox Engine on Amlogic S905X

This step-by-step tutorial guides you from extracted Pioneer XDJ-RX3 assets to running the native standalone `rbp` player on an **Amlogic S905X TV Box or SBC** running **Armbian aarch64**, complete with an HDMI display, USB capacitive touch, and a **Pioneer DDJ-FLX6** (4-channel) or **DDJ-FLX4** (2-channel) controller.

> [!NOTE]
> This project focuses on **Amlogic S905X** hardware. It builds upon the shims and concepts introduced by [salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b) and the performance mod suite by [@dj_ventotto](https://www.instagram.com/dj_ventotto/).
> Firmware acquisition and decryption are not covered here; see [docs/04-firmware-assets.md](docs/04-firmware-assets.md) for required asset structure.

---

## 0. Prerequisites & Hardware Requirements

### 1. Target Hardware (Amlogic S905X)
* **Board:** Amlogic S905X / S905X-W / S905L TV Box (e.g. TX3 Mini, X96 Mini) or SBC (Libre Computer Le Potato) with 1 GB or 2 GB RAM.
* **Operating System:** Armbian aarch64 (Linux 5.15+ kernel) flashed to eMMC or high-speed microSD.
* **Display:** HDMI monitor or LCD panel (native 1280×800 recommended).
* **DJ Controller:** Pioneer DDJ-FLX6 or DDJ-FLX4 connected via USB.
* **USB Storage:** USB flash drive exported from rekordbox (FAT32 or exFAT).

### 2. Workstation (Host Build Machine)
* Linux PC or Windows with WSL2 (Ubuntu / Debian).
* Cross-compilation tools: `arm-linux-gnueabi-gcc`, `libc6-dev-armel-cross`, `python3`, `make`, `tar`.

---

## 1. Verify Amlogic S905X Compatibility

Connect to your S905X board via SSH:
```sh
ssh root@192.168.137.22
```

Ensure essential utilities are installed on the board:
```sh
apt-get update && apt-get install -y gcc make python3 alsa-utils evtest fbset
```

Verify that the Armbian aarch64 kernel has **32-bit EL0 Support** enabled:
```sh
dmesg | grep -i '32-bit EL0'
```
Expected output:
```
[    0.000000] 32-bit EL0 Support
```

Check the framebuffer device:
```sh
fbset -fb /dev/fb0
```
It should report `1280x800` (or `1920x1080` if standard monitor). To force native 1280×800 mode:
```sh
fbset -fb /dev/fb0 -g 1280 800 1280 800 16
```

---

## 2. Build the Patched DirectFB 1.4 Stack (Host)

Follow [tools/build-directfb/README.md](tools/build-directfb/README.md) on your build machine:
1. Clones DirectFB 1.4.16.
2. Applies `directfb-full.diff` (single-buffer fbdev driver and multi-plane DRM integration).
3. Compiles and installs the ARM32 soft-float libraries into `work/dfb/lib/`.

---

## 3. Assemble the Runtime Chroot (Host)

Run [`scripts/build-chroot.sh`](scripts/build-chroot.sh) on your build host:
```sh
RX3=/path/to/extracted/XDJRX3-rootfs DFB="$PWD/work/dfb" scripts/build-chroot.sh
```
This script:
1. Merges the soft-float ARM32 rootfs, GUI assets, and patched `rbp` executable.
2. Compiles all shims from `scripts/shims/` (`fbshim.so`, `knobshim.so`, `audioshim.so`, `netshim.so`, `crashcatch.so`).
3. Installs `librx3_core.so` and the mod suite from `scripts/mods/`.
4. Produces the deployable archive: `work/rbx3-s905x.tgz`.

---

## 4. Deploy to the S905X Board

Transfer the archive and installation scripts to the board:
```sh
scp work/rbx3-s905x.tgz root@192.168.137.22:/tmp/
scp -r scripts/device    root@192.168.137.22:/tmp/
```

Execute the installer on the target:
```sh
ssh root@192.168.137.22 "sh /tmp/device/install.sh /tmp/rbx3-s905x.tgz"
```

The installer:
* Extracts the rootfs to `/opt/rblive4/rbx3-run`.
* Verifies 32-bit execution by running a test binary inside the chroot.
* Installs and enables the systemd service: `rblive4.service`.
* Prepares `/opt/rblive4/rb.conf` and `/opt/rblive4/mods.conf`.

---

## 5. Configure Hardware & Mods

Edit `/opt/rblive4/rb.conf` on the board to match your setup:

### Audio & Controller
```ini
# Controller selection: 'flx6' for DDJ-FLX6 (4-Ch) or 'flx4' for DDJ-FLX4 (2-Ch)
MIDI_MAP=flx6

# Low-latency ALSA hardware device
AUDIO_DEVICE=hw:CARD=DDJFLX6,DEV=0

# Framebuffer resolution
FB_WIDTH=1280
FB_HEIGHT=800
FB_BPP=16
```

### Performance Mods Configuration
Edit `/opt/rblive4/mods.conf` to enable or disable individual performance modules:
```ini
MOD_CORE=1                  # Core performance hook (librx3_core.so)
MOD_KEYSHIFT=1              # Harmonic mixing and pitch shift
MOD_KEY_SYNC=1              # Key sync with master deck
MOD_STEMS=1                 # Real-time stems separation
MOD_THEME_WHITE=1           # White bright theme support
MOD_BEATJUMP_CUSTOM=1       # Extended beatjump pads (8..64 bars)
MOD_RMX_ROLL=0              # Keep 0 for stability
```

---

## 6. Launch & Verification

Start the player service:
```sh
systemctl start rblive4
```

Monitor live logs:
```sh
journalctl -u rblive4 -f
```

Check the active process and display:
```sh
systemctl status rblive4
```

### What You Should See:
1. **Screen:**
   - The native Pioneer XDJ-RX3 waveform interface renders at 1280×800.
   - The top header displays: `KEY | STEMS | RMX | MODS | [Wi-Fi/BT]`.
   - Native controls **ZOOM, GRID, STATUS & BEAT FX** remain untouched and visible at the bottom right.
2. **Shortcuts & Themes:**
   - Open SHORTCUT screen (`browse_mode == 10`).
   - The **JOG LCD** card is replaced by the authentic **THEME** card: touch `[ LIGHT ]` to switch to light mode, or `[ DARK ]` to return to dark mode.
3. **Controller:**
   - Jog wheels, pitch faders, play/cue buttons, and channel strips on your DDJ-FLX6 or DDJ-FLX4 respond with zero latency.
   - Audio streams out of the controller's Master RCA and Headphone jacks.

---

## 7. Fast Iteration (Modifying Shims)

To modify and recompile a shim directly on the board without rebuilding the full chroot:
```sh
# On target board:
cd /opt/rblive4/shims_build/shims
make RX3=/opt/rblive4/rbx3-run fbshim.so
cp fbshim.so /opt/rblive4/fbshim.so
cp fbshim.so /opt/rblive4/rbx3-run/usr/lib/fbshim.so
systemctl restart rblive4
```
