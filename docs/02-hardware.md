# 02 — Hardware & Environment Architecture

This document compares the hardware architectures across five platforms relevant to this project: the source **Pioneer XDJ-RX3**, the early proof-of-concept targets (**Prime GO** and **SC Live 4**), the **Raspberry Pi 4B** reference, and the primary target: **Amlogic S905X**.

---

## 1. Hardware Architecture Matrix

| Specification | Pioneer XDJ-RX3 (Source) | Denon SC Live 4 (Legacy) | Raspberry Pi 4B (Reference) | Amlogic S905X (**Primary Target**) |
|---|---|---|---|---|
| **SoC** | NXP i.MX6 Quad (ARMv7) | Rockchip RK3288 (ARMv7) | Broadcom BCM2711 (Cortex-A72) | **Amlogic S905X / S905X-W (Meson GXL)** |
| **CPU Cores** | 4× Cortex-A9 @ 1.0 GHz | 4× Cortex-A17 @ 1.4 GHz | 4× Cortex-A72 @ 1.5–1.8 GHz | **4× Cortex-A53 @ 1.5 GHz** |
| **Float ABI** | **Soft-float** (EABI) | Hard-float (armhf) | Hard-float (armhf / aarch64) | **Hard-float / aarch64 with 32-bit EL0** |
| **Kernel** | Linux 3.0.101 (32-bit) | Linux 6.1.111 PREEMPT_RT | Linux 6.1+ (Pi OS 32-bit / 64-bit) | **Linux 5.15+ / 6.x Armbian (aarch64)** |
| **Display** | 1280×800 landscape, RGB565 | 800×1280 portrait (RGB32) | HDMI `vc4drmfb` (1280×800) | **HDMI `meson-drm` (1280×800 RGB565/32)** |
| **Touchscreen** | SPI-attached `tsc2007` resistive | ILI2117 capacitive (`event0`) | USB Touch Panel / evdev pointer | **USB Capacitive Touch Panel (CTP, evdev)** |
| **Audio Interface**| 3× CS4344 DACs | JP21 internal codec (8-ch) | DDJ-FLX4 USB Audio (`S24_3LE`) | **DDJ-FLX6 (4-Ch) / DDJ-FLX4 USB Audio** |
| **DJ Surface** | EUP / SUB MCUs over SPI | InMusic JP21 internal MIDI | DDJ-FLX4 USB MIDI (2-Ch) | **Pioneer DDJ-FLX6 (4-Ch) / DDJ-FLX4** |
| **USB Connectivity**| 2× Host + Sub-MCU | USB-A 2.0 Host | 2× USB 3.0 + 2× USB 2.0 | **2× to 4× USB 2.0 Host (DWC2)** |
| **Target Unit Cost**| ~$2,100 (complete console) | ~$1,300 (complete console) | ~$55–$85 (SBC only) | **~$15–$25 (Complete TV Box with enclosure)** |

---

## 2. Why Amlogic S905X is the Primary Target

**The Amlogic S905X column is the primary target** for **RBP-Amlogic-S905X**. The key architectural factors that make this platform ideal are:

### 1. Hardware 32-bit EL0 Support on 64-bit Cortex-A53
The ARM Cortex-A53 cores natively support AArch32 execution at Exception Level 0 (EL0). When running a 64-bit Armbian kernel built with `CONFIG_COMPAT=y`, the processor directly switches hardware execution modes to run 32-bit ARM binaries at bare-metal clock speeds. No CPU emulation overhead is incurred.

### 2. Native 1280×800 Landscape HDMI Output
Unlike the Denon SC Live 4 (which utilized an 800×1280 portrait display requiring a continuous 90° software rotation matrix that consumed substantial CPU cycles), Amlogic's `meson-drm` driver directly outputs 1280×800 landscape over HDMI. This matches the exact pixel dimensions of `rbp`'s internal GUI buffer, enabling zero-copy 1:1 scanout.

### 3. Dedicated USB Host Stack
Amlogic's DWC2 USB 2.0 controller reliably negotiates isochronous and bulk audio/MIDI transfers with Pioneer controllers. It delivers real-time 4-channel audio packets (`S24_3LE` format, 48 kHz, 64-frame buffer) with zero dropped frames.

### 4. Cost and Availability
Tens of millions of S905X Android TV boxes are available globally for \$15–\$25. These devices come complete with an injection-molded enclosure, 5V DC power adapter, HDMI port, USB ports, and integrated Wi-Fi/Ethernet, making them the most cost-effective standalone DJ platform in existence.

---

## 3. Shims Architecture: Overcoming Hardware Discrepancies

Because the Amlogic S905X TV box contains no proprietary Pioneer microcontrollers or SPI buses, the following userland shims translate system calls:

1. **`fbshim.so` (Display & Touch):**
   - Implements shadow double-buffering to eliminate 60Hz flickering on single-buffered DirectFB outputs.
   - Intercepts evdev touch events from USB touch panels (`wch.cn USB2IIC_CTP_CONTROL`, Goodix) and synthesizes the fake `/dev/tsc2007_2-0048` protocol expected by `rbp`.
   - Injects the Top Bar Mod Navigation (`KEY`, `STEMS`, `RMX`, `MODS`) and SHORTCUT Theme Toggle card.
2. **`knobshim.so` (Control Surface):**
   - Intercepts raw MIDI messages from the Pioneer DDJ-FLX6 (4-channel) or DDJ-FLX4 (2-channel).
   - Maps jog wheels, pitch faders, channel strips, and FX encoders directly to Pioneer `sendKey()` ABI functions.
   - Includes DSP anti-glitch protection to silence Beat FX before channel switches.
3. **`audioshim.so` (Audio Engine):**
   - Captures ALSA stream allocations from `rbp` and routes Master and Cue channels to `hw:CARD=DDJFLX6` or `hw:CARD=DDJFLX4` in 24-bit little-endian format.
4. **`crashcatch.so` (Stability Guard):**
   - Catches benign segfaults or bus errors during ungraceful USB disconnections, keeping the main process resilient.
