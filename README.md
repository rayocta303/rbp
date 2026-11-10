# RBP-Amlogic-S905X

**Standalone Pioneer DJ XDJ-RX3 rekordbox Player on Low-Cost Amlogic S905X TV Boxes and SBCs**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Amlogic S905X](https://img.shields.io/badge/Platform-Amlogic%20S905X%20(Armbian%20aarch64)-orange.svg)](docs/25-amlogic-s905x.md)
[![Base: salesforcedoc/rbpi4b](https://img.shields.io/badge/Base-salesforcedoc%2Frbpi4b-blueviolet.svg)](https://github.com/salesforcedoc/rbpi4b)
[![Mods: dj_ventotto](https://img.shields.io/badge/Mods-dj__ventotto-E4405F.svg?logo=instagram)](https://www.instagram.com/dj_ventotto/)

---

## Overview

**RBP-Amlogic-S905X** is an embedded software environment and userland shim suite that enables running the native standalone **Pioneer DJ XDJ-RX3 rekordbox player binary** (`rbp`, internally named `rb`, extracted from official firmware v1.20) on affordable **Amlogic S905X-series Android TV Boxes and Single Board Computers** (TX3 Mini, X96 Mini, Tanix TX3, Libre Computer Le Potato, etc.) running **Armbian aarch64** (Linux kernel 5.15+).

The native soft-float ARM32 `rbp` executable runs directly on the Cortex-A53 CPU via the 64-bit kernel's hardware **32-bit EL0 Execution Support** (`CONFIG_COMPAT`), eliminating the need for slow CPU emulation. Thin, high-performance C shims seamlessly bridge standard Linux subsystem interfaces (HDMI Framebuffer, ALSA Audio, USB MIDI, Evdev Touchscreen) into the proprietary DirectFB and sub-MCU protocols expected by the Pioneer engine.

> [!IMPORTANT]
> **Interoperability & Digital Preservation Notice:**  
> This repository contains **NO copyrighted Pioneer DJ / AlphaTheta firmware binaries, no `rbp` executable, no firmware decryption keys, and no proprietary music databases.** Users must legally provide their own extracted firmware assets. See [NOTICE.md](NOTICE.md).

---

## Credits & Acknowledgements

This project is an independent standalone repository built with sincere appreciation for the open-source engineering and reverse engineering work of:

* **[salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b)** by `@salesforcedoc` / `@i.erhan.es`:
  Provided the foundational concept and base shims (`fbshim`, `knobshim`, `audioshim`, `crashcatch`), DirectFB 1.4 fbdev patches, and fake tsc2007 touch pipeline that initially demonstrated running `rbp` on Raspberry Pi 4B.
* **[@dj_ventotto](https://www.instagram.com/dj_ventotto/)**:
  Creator of the advanced XDJ-RX3 performance modifications suite (`librx3_core.so`, real-time Stems separation, Harmonic Mixing / Keyshift, White Bright Theme, Custom BeatJump, and extended pad widgets).

---

## Why Amlogic S905X?

| Feature | Amlogic S905X (Meson GXL) | Typical Alternatives (e.g. Pi 4) |
|---|---|---|
| **Hardware Cost** | **\$15 – \$25** (Widely available surplus TV boxes) | \$55 – \$85+ (Plus cooler, case, power supply) |
| **Form Factor** | Compact, enclosed plastic chassis with integrated 5V PSU | Bare board requiring external enclosure |
| **CPU Architecture** | Quad-Core ARM Cortex-A53 @ 1.5 GHz with full 32-bit EL0 Support | Quad-Core ARM Cortex-A72 @ 1.8 GHz |
| **Graphics & Display** | `meson-drm` driving HDMI 1280×800 60Hz natively | `vc4-kms-v3d` driving HDMI |
| **USB Connectivity** | 2× USB 2.0 High-Speed Host ports (ideal for controller + USB drive) | USB 2.0 / 3.0 ports |
| **Operating System** | Armbian aarch64 (Debian Bookworm / Ubuntu Jammy base) | Raspberry Pi OS |

---

## Core Capabilities & Features

### 1. Controllers & Mixing Surfaces
* **Pioneer DDJ-FLX6 (4-Channel):**
  - Full 4-channel deck control with fast Deck 1/2 mirroring for multi-layer sets.
  - High-resolution 720 PPR jog wheels with calibrated vinyl touch sensitivity and pitch bend.
  - 6 Beat FX Slot Racks with automated DSP anti-glitch protection (auto-disengages FX before channel switch).
  - Merge FX rotary encoder mapped to Beat FX Depth & Engage.
  - Sound Color FX quick-select via `SHIFT` + `CUE 1–4` (Filter, Dub Echo, Noise, Space).
* **Pioneer DDJ-FLX4 (2-Channel):**
  - Complete 2-channel control surface mapping with jog touch, tempo sliders, loops, and EQ strips.
* **Denon JP21 & Keyboard Fallback:**
  - Support for legacy test setups and direct PC keyboard mapping.

### 2. Audio Subsystem
* **Low-Latency 4-Channel USB Audio:**
  - ALSA hardware negotiation (`hw:CARD=DDJFLX6` or `hw:CARD=DDJFLX4`) in `S24_3LE` format.
  - Real-time Master RCA output (Ch 1/2) and independent Headphone Cue monitoring (Ch 3/4).
  - Real-time block transfers (64 frames / 768 bytes) with zero buffer underruns.
* **Advisory HDMI Audio Mirror:**
  - Non-blocking secondary audio mirror routed directly to the HDMI display monitor.

### 3. Display & Touchscreen Pipeline
* **Pixel-Perfect 1280×800 Display:**
  - Custom DirectFB 1.4 fbdev engine configured for single-buffered, zero-scaling native output.
* **Flicker-Free Double Buffering:**
  - `fbshim.so` shadow framebuffer architecture ensures atomic VSYNC presentation without horizontal tearing.
* **USB Capacitive Touch (CTP):**
  - Instant plug-and-play support for popular USB capacitive touch controllers (`wch.cn USB2IIC_CTP_CONTROL`, Goodix, FocalTech) converted to Pioneer's fake `tsc2007` protocol.

### 4. Custom UI & Performance Enhancements
* **Top Header Mod Navigation:**
  - Integrated top bar mod quick-select buttons (`KEY`, `STEMS`, `RMX`, `MODS`) located at `y: 8..44`.
  - Moving custom controls to the top bar leaves native **ZOOM, GRID, STATUS & BEAT FX** buttons 100% visible and responsive.
* **Harmonious SHORTCUT Theme Card:**
  - Overwrites the unpopulated **JOG LCD** card on the native SHORTCUT screen (`browse_mode == 10`) with an authentic Pioneer-styled **THEME** toggle card (`[ DARK ]` and `[ LIGHT ]`).
* **Clean Wi-Fi & Bluetooth Top Strip:**
  - Anti-aliased white glyphs with subtle vertical dividers, brief electric blue touch feedback, and strictly no background boxes.
  - Automatically hides when the swipe-down menu is active to avoid overlapping the native `MENU` button.
* **Mod Menu (Tab 3):**
  - In-system status overview of all active modules with a direct **RESTART RBP** action hooked into systemd (`rblive4.service`).

---

## Repository Structure

```
.
├── docs/                     # Technical documentation & architectural specifications
│   ├── 00-overview.md        # Architecture overview & why S905X works
│   ├── 06-display.md         # DirectFB 1.4 fbdev display driver & multi-plane DRM
│   ├── 07-touch.md           # Touch pointer & fake tsc2007 driver
│   ├── 08-controls.md        # Control surface mapping & LED feedback
│   ├── 09-audio.md           # ALSA 4-channel audio pipeline & S24_3LE format
│   ├── 25-amlogic-s905x.md   # Dedicated Amlogic S905X bring-up guide
│   ├── 26-flx6-midi.md       # Pioneer DDJ-FLX6 (4-Ch) mapping documentation
│   └── ...
├── scripts/
│   ├── build-chroot.sh       # Assembles the soft-float ARM32 execution chroot
│   ├── device/               # On-device runtime scripts
│   │   ├── load-mods.sh      # Environment injector for librx3_core & mod suite
│   │   ├── start-rb.sh       # Main player execution supervisor & hotplug monitor
│   │   ├── rblive4.service   # Systemd service unit for automated boot
│   │   └── rb.conf           # System configuration file (Audio, Display, MIDI)
│   ├── mods/                 # Upstream XDJ-RX3 mods (core, stems, theme, beatjump)
│   └── shims/                # Userland translation shims
│       ├── fb_shim.c         # Framebuffer, shadow VSYNC blit, and touch capture
│       ├── knob_shim.c       # USB MIDI sequencer → rbp keycode mapper
│       ├── audio_shim.c      # ALSA low-latency audio router
│       ├── net_paint.c       # Native-styled UI overlay painter & top header buttons
│       ├── net_ui.c          # Overlay IPC coordinator and double-buffer presenter
│       └── ui_overlay.c      # SHORTCUT screen Theme card & Utility IP display
├── tools/                    # Host utilities & Pro DJ Link traffic monitor
├── NOTICE.md                 # Provenance, credits, and legal notice
├── TUTORIAL.md               # Step-by-step installation and bring-up manual
└── LICENSE                   # MIT Open Source License
```

---

## Quick Start Guide

Detailed step-by-step instructions are available in [**TUTORIAL.md**](TUTORIAL.md).

### 1. Requirements
* **Hardware:** Amlogic S905X TV Box or SBC with at least 1 GB RAM, flashed with **Armbian aarch64** (Linux 5.15+).
* **Display:** HDMI monitor or 7"/10.1" touchscreen capable of 1280×800 resolution.
* **DJ Controller:** Pioneer DDJ-FLX6 or DDJ-FLX4 connected via USB.
* **Firmware Assets:** Decrypted and extracted XDJ-RX3 rootfs assets (see [docs/04-firmware-assets.md](docs/04-firmware-assets.md)).

### 2. Verify 32-bit EL0 Support on S905X
Log in to your S905X device via SSH and verify that your kernel supports 32-bit userspace execution:
```sh
dmesg | grep -i '32-bit EL0'
# Expected output:
# [    0.000000] 32-bit EL0 Support
```

### 3. Deploy and Run
Transfer your prepared chroot archive and device scripts to the TV box:
```sh
scp work/rbx3-s905x.tgz root@192.168.137.22:/tmp/
scp -r scripts/device root@192.168.137.22:/tmp/
ssh root@192.168.137.22 "sh /tmp/device/install.sh /tmp/rbx3-s905x.tgz"
```

Start the systemd player service:
```sh
systemctl start rblive4
systemctl status rblive4
```

---

## Documentation Quick Links

* [**TUTORIAL.md** — Step-by-step Installation & Bring-up Guide](TUTORIAL.md)
* [**docs/25-amlogic-s905x.md** — Amlogic S905X Hardware & Kernel Configuration](docs/25-amlogic-s905x.md)
* [**docs/26-flx6-midi.md** — Pioneer DDJ-FLX6 (4-Ch) Control Surface Mapping](docs/26-flx6-midi.md)
* [**docs/09-audio.md** — ALSA 4-Channel Audio Engine & Low-Latency Tuning](docs/09-audio.md)
* [**docs/06-display.md** — DirectFB fbdev Display Stack & Framebuffer Mechanics](docs/06-display.md)
* [**docs/07-touch.md** — Touchscreen Translation & Fake tsc2007 Driver](docs/07-touch.md)
* [**NOTICE.md** — Legal, Trademarks, and Third-Party Licenses](NOTICE.md)

---

## Contributing & License

Contributions, hardware reports, and controller mappings are welcome!  
This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for details.
