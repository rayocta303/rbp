# 25 — Amlogic S905X Target Architecture & Bring-Up

Documentation for running the Pioneer DJ XDJ-RX3 standalone rekordbox player (`rbp`) on **Amlogic S905X / S905X-series (Meson GXL/G12A/SM1)** TV Boxes and SBCs under **Armbian aarch64 (Linux 5.15+ / 6.x)**.

---

## 1. Why Amlogic S905X?

The Amlogic S905X (and its siblings S905X-B, S905L, S905W, S905X2, S905X3, S905X4) is one of the most widely available, low-cost ARM SoCs in the world, found in tens of millions of Android TV boxes (TX3 Mini, X96 Mini, Tanix TX3, HK1 Box) and developer boards (Libre Computer Le Potato, Khadas VIM1/VIM2/VIM3, ODROID-C2/C4).

| Specification | Raspberry Pi 4B | Amlogic S905X (e.g. TX3 Mini / Le Potato) |
|---|---|---|
| **SoC** | Broadcom BCM2711 | Amlogic S905X (Meson GXL) |
| **CPU Cores** | 4× Cortex-A72 @ 1.5–1.8 GHz | 4× Cortex-A53 @ 1.2–1.5 GHz |
| **Architecture** | ARMv8-A (AArch64) | ARMv8-A (AArch64) |
| **32-bit EL0 Support** | Native in kernel (`COMPAT`) | Native in kernel (`CONFIG_COMPAT=y`) |
| **GPU / Video** | VideoCore VI (vc4) | ARM Mali-450 MP3 (lima) |
| **Display Output** | Micro-HDMI (1280×800 fbdev forced) | Full-size HDMI 2.0a (`meson-drm` / `fbdev`) |
| **USB Host** | 2× USB 3.0 + 2× USB 2.0 (VL805) | 2× to 4× USB 2.0 Host (Dwcotg / DWC2) |
| **Ethernet / Wi-Fi** | Gigabit + onboard Cypress Wi-Fi | 100M Ethernet + RTL8189 / RTL8723 / SSV Wi-Fi |
| **Form Factor / Cost** | SBC (~$45–$75) | TV Box with case & power supply (~$15–$30) |

Like the Pi 4, the S905X's Cortex-A53 cores support **AArch32 execution at Exception Level 0 (EL0)** natively when compiled with `CONFIG_COMPAT=y`. This means the soft-float ARM32 `rbp` executable from the XDJ-RX3 firmware runs **directly on bare metal silicon without CPU emulation**, delivering maximum DSP throughput and rock-solid audio timing.

---

## 2. Kernel & OS Requirements

### Operating System
- **Distribution**: Armbian (Bullseye / Bookworm) or Ophub Armbian build.
- **Kernel**: Linux mainline `5.15.y` (LTS), `6.1.y`, or `6.6.y` aarch64.
- **Device Tree (DTB)**:
  - Standard S905X 1GB/2GB: `meson-gxl-s905x-p212.dtb` or board-specific DTB (`meson-gxl-s905x-tx3-mini.dtb`, `meson-gxl-s905x-libretech-cc.dtb`).

### Verifying 32-bit EL0 Compatibility
Log into the S905X unit via SSH and verify that the kernel supports 32-bit userspace execution:
```sh
# 1. Verify kernel architecture is aarch64
uname -m
# -> aarch64

# 2. Check 32-bit EL0 support in dmesg
dmesg | grep -i "32-bit EL0"
# -> [    0.000000] 32-bit EL0 Support

# 3. Check CONFIG_COMPAT
zcat /proc/config.gz | grep CONFIG_COMPAT
# -> CONFIG_COMPAT=y
```

---

## 3. Subsystem Architecture on S905X

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Amlogic S905X Host (Armbian aarch64)                │
│  Meson GXL · HDMI 1280×800 · USB 2.0 DWC2 Host · USB Wi-Fi / Eth     │
│  DDJ-FLX6 or DDJ-FLX4 on USB Host (4-channel Audio + MIDI Sequencer)  │
│                                                                        │
│  ┌────────────── /opt/rblive4/rbx3-run (chroot) ─────────────────────┐ │
│  │  soft-float glibc 2.13 + Pioneer RX3 libs + DirectFB 1.4.16       │ │
│  │                                                                   │ │
│  │   rbp (XDJ-RX3 Rekordbox Standalone Player)                       │ │
│  │      ▲        ▲           ▲            ▲            ▲             │ │
│  │      │        │           │            │            │             │ │
│  │  netshim  knobshim    audioshim     fbshim     crashcatch         │ │
│  │  (WLAN/ETH (FLX6/FLX4   (Low-latency (1280x800   (SIGSEGV/BUS     │ │
│  │   ProDJLink) MIDI Map)   ALSA Audio)  DirectFB)   Guard)          │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│         ▲            ▲               ▲                   ▲             │
│     wlan0/eth0   /dev/snd/pcmC0   /dev/fb0        /dev/input/event*    │
│   (Pro DJ Link)  (FLX6 USB Audio) (Meson DRM FB)  (USB/I2C Touch CTP)  │
└────────────────────────────────────────────────────────────────────────┘
```

### 1. Display (`/dev/fb0`)
- S905X HDMI is driven by `meson-drm`.
- The display is initialized at `1280x800` (native XDJ-RX3 UI resolution) by forcing kernel display parameters in `/boot/uEnv.txt` or `/boot/armbianEnv.txt`:
  ```ini
  extraargs=video=HDMI-A-1:1280x800@60
  ```
- DirectFB writes directly to single-page `/dev/fb0` with no desktop compositor interference.

### 2. Audio (`audioshim.so`)
- The USB audio interface of DDJ-FLX6 or DDJ-FLX4 is exposed via ALSA:
  - DDJ-FLX6: `hw:CARD=DDJFLX6,DEV=0`
  - DDJ-FLX4: `hw:CARD=DDJFLX4,DEV=0`
- `audioshim.so` sets format `S24_3LE`, 44.1 kHz, 4 channels (Channels 1–2: Master Out, Channels 3–4: Headphone/Cue Out).
- DMA buffer periods are optimized to 64–128 frames per chunk, maintaining < 5ms audio latency on the S905X USB host.

### 3. Controls (`knobshim.so`)
- Driven via ALSA MIDI Sequencer (`/dev/snd/seq`).
- Automatically scans and attaches to the connected controller based on `MIDI_MAP`:
  - `MIDI_MAP=flx6` for Pioneer DDJ-FLX6 / FLX6-GT
  - `MIDI_MAP=flx4` for Pioneer DDJ-FLX4
  - `MIDI_MAP=jp21` for Denon SC Live 4 / Prime GO
  - `MIDI_MAP=kbd` for USB Keyboard fallback
- Translates hardware MIDI inputs into `rbp`'s internal `KeyManager::asEventCode` keycodes.

### 4. Touchscreen & Pointer (`fbshim.so`)
- Supports standard USB Capacitive Touch Panels (such as `wch.cn USB2IIC_CTP_CONTROL`, Goodix, Ilitek, FT5406).
- Evdev touch events are intercepted, normalized, and converted into emulated TI TSC2007 I2C touch packets expected by `rbp`.

### 5. Pro DJ Link over Wi-Fi (`netshim.so`)
- S905X TV boxes typically connect via onboard Wi-Fi (`wlan0`).
- `netshim.so` hooks socket and interface calls (`getifaddrs`, `ioctl(SIOCGIFNAME)`), translating `eth0` requests to `wlan0`.
- Automated Pro DJ Link trigger broadcasts discovery packets upon `rbp` boot, instantly linking with Rekordbox on PC/Mac or other CDJ units.

---

## 4. Setup & Deployment on S905X

### Step 1: Prepare Rootfs & Chroot
On the S905X box, the deployment directory is `/opt/rblive4`.
Ensure the RX3 extracted chroot is placed in `/opt/rblive4/rbx3-run`.

### Step 2: Compile Shims against Rootfs
When compiling shims directly on the S905X unit:
```sh
cd /opt/rblive4_build/src/scripts/shims
make clean
make RX3=/opt/rblive4_build/rootfs
```
> **CRITICAL**: The `RX3=/opt/rblive4_build/rootfs` argument is mandatory so that the linker resolves `libc` symbols against the soft-float glibc-2.13 RX3 rootfs (`GLIBC_2.4` and `GLIBC_2.7`) instead of the host's modern glibc (`GLIBC_2.34`).

### Step 3: Install Shims
```sh
cp -v knobshim.so audioshim.so fbshim.so netshim.so crashcatch.so /opt/rblive4/
cp -v knobshim.so audioshim.so fbshim.so netshim.so crashcatch.so /opt/rblive4/rbx3-run/usr/lib/
```

### Step 4: Configure `rb.conf`
Edit `/opt/rblive4/rb.conf` to set your controller and device mapping:
```ini
# For DDJ-FLX6:
AUDIO_DEV=hw:CARD=DDJFLX6,DEV=0
MIDI_MAP=flx6

# Or for DDJ-FLX4:
# AUDIO_DEV=hw:CARD=DDJFLX4,DEV=0
# MIDI_MAP=flx4

# Pointing device
POINT_KIND=auto
```

### Step 5: Start Service
```sh
systemctl restart rblive4
systemctl status rblive4
```
Check `/opt/rblive4/log/rbp.log` and `/tmp/knobshim.log` to verify controller connection, engine startup, and audio streaming.
