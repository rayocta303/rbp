# 01 — Hardware Survey & Historical Lineage

This document records the hardware environment and system characteristics of the target hardware for **RBP-Amlogic-S905X**, alongside historical survey data from the reference devices (Denon SC Live 4 / Prime GO) where the soft-float `rbp` runtime was first reverse-engineered.

---

## 1. Primary Target Hardware Survey: Amlogic S905X

The following survey was captured directly on a live Amlogic S905X TV Box running Armbian Linux aarch64 over SSH:

### A. Operating System & Kernel
```sh
# uname -a
Linux unx.dj 5.15.201-ophub #1 SMP PREEMPT Wed Mar 4 07:13:22 UTC 2026 aarch64 GNU/Linux

# cat /etc/os-release
PRETTY_NAME="Armbian 24.5.0-trunk Bookworm"
NAME="Debian GNU/Linux"
VERSION_ID="12"
VERSION="12 (bookworm)"
ID=debian
HOME_URL="https://www.armbian.com"
```

* **SoC:** Amlogic S905X (Meson GXL architecture, revision `Amlogic Meson GXL (S905X) rev d`).
* **CPU:** Quad-Core ARM Cortex-A53 @ 1.51 GHz (ARMv8-A 64-bit architecture with hardware AArch32 support).
* **Kernel:** Linux `5.15.201-ophub`, PREEMPT, `aarch64`.
* **32-bit EL0 Execution Support:**
  ```sh
  # dmesg | grep -i "32-bit EL0"
  [    0.000000] 32-bit EL0 Support
  ```
  *Crucial:* Enables running soft-float ARM32 binaries directly on the CPU without qemu or dynamic binary translation.

### B. Display Subsystem (`meson-drm`)
```sh
# fbset -fb /dev/fb0
mode "1280x800-60"
    # D: 71.000 MHz, H: 49.306 kHz, V: 59.910 Hz
    geometry 1280 800 1280 800 16
    timings 14085 48 32 6 3 80 23
    rgba 5/11,6/5,5/0,0/0
endmode
```
* **Driver:** `meson-drm` (Linux DRM/KMS fbdev emulation).
* **Resolution:** 1280×800 at 16 bpp (RGB565) — identical to the native Pioneer XDJ-RX3 physical screen!
* **Presentation:** Single-buffered scanout with double-buffering managed via `fbshim.so`.

### C. USB Controller & Peripherals
* **Controller:** Synopsys DesignWare Core 2 (DWC2) USB 2.0 Host Controller (`dwc2`).
* **Audio Device:** Pioneer DDJ-FLX6 or DDJ-FLX4 negotiated in 4-channel `S24_3LE` mode via ALSA hardware endpoints.
* **Touch Controller:** USB Capacitive Touch Panel (`wch.cn USB2IIC_CTP_CONTROL`, Goodix, or FocalTech) reporting standard multi-touch evdev events.

---

## 2. Historical Reference Survey: Denon DJ SC Live 4 (`JP21`)

> [!NOTE]
> The data below is preserved for historical and comparative reference from the initial [salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b) and PrimeBox research that established the soft-float chroot approach.

```
NAME=Buildroot
VERSION=2023.02.11-5-g140647e9b3
Linux sclive4 6.1.111-inmusic-2024-09-19-rt41 #1 SMP PREEMPT_RT armv7l GNU/Linux
```

* **CPU:** Rockchip RK3288 (ARMv7 Cortex-A17 hard-float 32-bit).
* **Display:** 800×1280 portrait DRM display (required 90° clockwise rotation in DirectFB).
* **Controls:** InMusic proprietary JP21 ALSA MIDI control surface.
* **Audio:** 8-channel internal codec (`hw:1,0`).

Comparing the two targets reveals why **Amlogic S905X** is superior for a standalone DJ setup:
1. Native landscape 1280×800 HDMI output eliminates the CPU-intensive 90° rotation pass required by portrait screens.
2. The 64-bit Cortex-A53 CPU runs mainline 5.15+ LTS kernels with clean `aarch64` userspace tools alongside the soft-float chroot.
3. Standard USB Host ports support consumer DJ controllers (DDJ-FLX6, DDJ-FLX4) without proprietary vendor buses.
