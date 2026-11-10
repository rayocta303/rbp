# 00 — Overview: RBP-Amlogic-S905X

This project runs the standalone **Pioneer DJ XDJ-RX3 rekordbox player** (`rbp`, internally called `rb`) on low-cost **Amlogic S905X-series TV Boxes and Single Board Computers (SBCs)** under **Armbian aarch64** (Linux 5.15+).

The native XDJ-RX3 firmware builds its core player application as a **soft-float ARM32** executable. The quad-core 64-bit Cortex-A53 processor in the Amlogic S905X executes soft-float EABI ELF binaries directly on bare metal silicon through the kernel's **32-bit EL0 Support** (`CONFIG_COMPAT`).

There is no CPU emulation. The genuine `rbp` executable executes at full native clock speed, with thin userland C shims translating standard Linux subsystems (HDMI framebuffer, ALSA audio, USB MIDI controller, USB capacitive touch pointer) into the proprietary interfaces expected by the Pioneer engine.

---

## Primary Architectural Pillars

1. **Target Board:** [25 — Amlogic S905X](25-amlogic-s905x.md) (Meson GXL TV Boxes / SBCs, Armbian aarch64, 32-bit EL0).
2. **Display Subsystem:** [06 — Display](06-display.md) (DirectFB 1.4 fbdev driver, pixel-accurate 1280×800 at 16 bpp, zero scaling).
3. **Control Surface:** [26 — DDJ-FLX6 MIDI](26-flx6-midi.md) (4-channel mixing, 720 PPR jogs, 6 Beat FX racks) and [15 — DDJ-FLX4 MIDI](15-flx4-midi.md) (2-channel).
4. **Audio Engine:** [09 — Audio](09-audio.md) (Low-latency ALSA 4-channel `S24_3LE`, Master RCA + Cue monitoring).
5. **UI & Navigation:** [28 — Network Settings & Mod UI](28-network-settings-overlay-plan.md) (Top header mod navigation, clean Wi-Fi/BT strip, and in-place SHORTCUT Theme card).

---

## System Architecture Diagram

```
┌────────────────────────────────────────────────────────────────────────────┐
│                  Target Hardware: Amlogic S905X (Meson GXL)                │
│  Quad-Core Cortex-A53 · Armbian aarch64 (Linux 5.15+ with 32-bit EL0)      │
│  HDMI 1280×800 Native · USB 2.0 Host · USB Capacitive Touch CTP            │
│  Pioneer DDJ-FLX6 (4-Ch) or DDJ-FLX4: Low-Latency ALSA Audio + USB MIDI    │
│                                                                            │
│  ┌───────────────── /opt/rblive4/rbx3-run (chroot) ───────────────────────┐│
│  │  soft-float glibc 2.13 + RX3 shared libraries + DirectFB 1.4.16        ││
│  │                                                                        ││
│  │   rbp (XDJ-RX3 Rekordbox Standalone Player Engine)                      ││
│  │      ▲        ▲           ▲            ▲            ▲                  ││
│  │      │        │           │            │            │                  ││
│  │  netshim   knobshim   audioshim     fbshim     crashcatch              ││
│  │  (ProDJ    (FLX6/FLX4 (ALSA 4-Ch    (1280x800   (SIGSEGV guard)        ││
│  │   Link)     MIDI Map)  S24_3LE)      Shadow FB)                        ││
│  │                                                                        ││
│  │   librx3_core.so (Performance Mods Hook Suite by @dj_ventotto)         ││
│  │   libdirectfb_fbdev.so (Single-page fbdev direct presentation)         ││
│  └────────────────────────────────────────────────────────────────────────┘│
│         ▲            ▲               ▲                   ▲                 │
│     wlan0/eth0   /dev/snd/seq    /dev/fb0        /dev/input/event*         │
│    (Pro DJ Link) (FLX6/FLX4 MIDI)(HDMI 1280x800) (USB/I2C Touch CTP)      │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Hardware Translation Matrix

| Subsystem | Native XDJ-RX3 Hardware | Amlogic S905X Implementation | Translation Method |
|---|---|---|---|
| **CPU ABI** | Soft-float ARM32 | Cortex-A53 (aarch64) | 32-bit EL0 kernel compat + soft-float glibc chroot |
| **Display** | 1280×800 RGB565 LVDS | HDMI 1280×800 60Hz | `meson-drm` (/dev/fb0) + patched DirectFB fbdev driver |
| **Touchscreen** | SPI-attached `tsc2007` | USB Capacitive Touch Panel (CTP) | `fbshim.so` converts evdev touch events to fake `tsc2007` protocol |
| **Controls** | Pioneer EUP/SUB front MCUs | Pioneer DDJ-FLX6 / DDJ-FLX4 | `knobshim.so` maps ALSA raw MIDI messages to internal `sendKey()` events |
| **Audio** | 3× Cirrus Logic CS4344 DACs | USB Audio (`hw:CARD=DDJFLX6`) | `audioshim.so` routes Master and Cue channels in `S24_3LE` format |
| **USB Media** | Sub-MCU media socket | USB-A 2.0 Host port | `start-rb.sh` handles auto-mount and DeviceSQL database import |
| **Pro DJ Link** | Dedicated Ethernet hardware | Wi-Fi (`wlan0`) or Ethernet (`eth0`) | `netshim.so` overrides interface binding for UDP ports 50000..50002 |

---

## Provenance & Attribution

This project is an independent implementation built upon:
* **[salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b)** by `@salesforcedoc` / `@i.erhan.es` (Base shims, DirectFB patches, and touch translation).
* **[@dj_ventotto](https://www.instagram.com/dj_ventotto/)** (XDJ-RX3 performance mod suite: `librx3_core.so`, Stems separation, Harmonic Mixing, Bright Theme).
