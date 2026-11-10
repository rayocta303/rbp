# docs/ — RBP-Amlogic-S905X Technical Documentation

Technical architectural specifications, bring-up manuals, hardware comparisons, and data dictionaries for **RBP-Amlogic-S905X**.

---

## Getting Started

* **New Bring-Up:** Start with [**25 — Amlogic S905X**](25-amlogic-s905x.md) for TV box hardware bring-up, Armbian kernel configuration, and DirectFB display setup.
* **4-Channel Controller:** See [**26 — DDJ-FLX6 MIDI**](26-flx6-midi.md) for mapping 4-channel strips, 720 PPR jogs, and 6 Beat FX racks.
* **Audio Engine:** See [**09 — Audio**](09-audio.md) for low-latency ALSA 4-channel routing (`S24_3LE`).
* **Display & VNC:** See [**24 — UI Layering and VNC**](24-ui-layering-and-vnc.md) for multi-plane DRM compositing and zero-copy VNC video mirroring.
* **Mod Navigation & Theme Card:** See [**28 — Network Settings & Mod UI**](28-network-settings-overlay-plan.md).

---

## Index of Technical Documents

| # | Document | Target / Description |
|---|---|---|
| 00 | [overview](00-overview.md) | High-level system architecture, why Amlogic S905X works, and the shim model |
| 01 | [device-survey](01-device-survey.md) | Historical Denon SC Live 4 hardware survey (reference baseline) |
| 02 | [hardware](02-hardware.md) | Hardware comparison: XDJ-RX3 vs Prime GO vs SC Live 4 vs S905X vs Pi 4 |
| 03 | [port-plan](03-port-plan.md) | Porting strategy, reuse map, and build dependencies |
| 04 | [firmware-assets](04-firmware-assets.md) | Firmware acquisition overview and required extracted asset layout |
| 05 | [chroot](05-chroot.md) | Soft-float glibc 2.13 runtime environment and multiarch compat |
| 06 | [display](06-display.md) | DirectFB fbdev driver: single-buffered present modes, DRM overlays, and `directfbrc` |
| 07 | [touch](07-touch.md) | Capacitive touch translation: evdev discovery, axis mapping, and fake tsc2007 protocol |
| 08 | [controls](08-controls.md) | Control surface abstraction: MIDI sequencer to rbp keycodes, LED feedback, and VU meters |
| 09 | [audio](09-audio.md) | Low-latency 4-channel USB audio engine, `S24_3LE` format negotiation, and HDMI audio mirror |
| 10 | [usb](10-usb.md) | rekordbox-analyzed USB storage auto-mounting and DeviceSQL database import |
| 11 | [runtime-launcher](11-runtime-launcher.md) | Launcher sequence, hotplug monitoring, and systemd service management |
| 12 | [troubleshooting](12-troubleshooting.md) | Comprehensive symptom → diagnostic → resolution runbook |
| 13 | [raspberrypi4](13-raspberrypi4.md) | Alternate target reference: Raspberry Pi 4B setup and vc4-kms bring-up |
| 15 | [flx4-midi](15-flx4-midi.md) | Pioneer DDJ-FLX4 (2-Channel) MIDI mapping tables and verification steps |
| 16 | [input-and-hotplug](16-input-and-hotplug.md) | Input handling edge cases: key release debounce, hot-swapping, and startup thump |
| 17 | [rx3-flx4-comparison](17-rx3-flx4-comparison.md) | Comparative analysis of sibling ports and firmware LED timing characteristics |
| 18 | [prodjlink](18-prodjlink.md) | Pro DJ Link integration (`netshim.so`): UDP discovery over wlan0/eth0 and packet filtering |
| 19 | [vnc](19-vnc.md) | `vncserve`: zero-copy framebuffer mirroring over VNC with touch feedback |
| 20 | [config-page](20-config-page.md) | Web configuration and diagnostic dashboard served on LAN port 5904 |
| 21 | [dfd-dataflow](21-dfd-dataflow.md) ([ID](21-dfd-dataflow-id.md)) | Comprehensive Data Flow Diagram (DFD): Level 0 Context, Level 1 Decomposition |
| 22 | [rbp-data-dictionary](22-rbp-data-dictionary.md) | Complete reverse-engineered Data Dictionary: ARM32 ABI, structs, keycodes, and memory offsets |
| 23 | [usb-hid-pc-plan](23-usb-hid-pc-plan.md) | Architectural roadmap for USB-B HID/MIDI PC connection (Hardware Unlock) |
| 24 | [ui-layering-and-vnc](24-ui-layering-and-vnc.md) | Multi-plane DRM overlay layering, hardware compositing, and VNC pipeline |
| 25 | [amlogic-s905x](25-amlogic-s905x.md) | **Primary Target:** Amlogic Meson GXL architecture, Armbian setup, 32-bit EL0 Support, and meson-drm |
| 26 | [flx6-midi](26-flx6-midi.md) | **Primary Controller:** Pioneer DDJ-FLX6 (4-Ch) mapping, 720 PPR jog, 6 Beat FX racks, Sound Color FX |
| 27 | [browse-and-waveform-mods](27-browse-and-waveform-mods.md) | UI enhancements: Touch Preview, Custom Columns, and Camelot notation |
| 28 | [network-settings-overlay-plan](28-network-settings-overlay-plan.md) | Top-bar mod buttons (`KEY`, `STEMS`, `RMX`, `MODS`), Wi-Fi/BT strip, and SHORTCUT Theme card |
