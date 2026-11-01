# Notice, copyright and legal

**XDJ-RX3 Standalone Engine** is an independent interoperability and preservation project.
It is **not affiliated with, endorsed by, or sponsored by** Pioneer DJ, AlphaTheta Corporation,
Amlogic, Inc., Raspberry Pi Ltd, Denon DJ, inMusic Brands, or any of their subsidiaries.

The goal is to run the standalone Pioneer DJ XDJ-RX3 rekordbox player (`rbp`, called `rb`
internally) on **Amlogic S905X-series TV Boxes/SBCs** (running Armbian aarch64) and the
**Raspberry Pi 4B**, using a soft-float-chroot + userland shim approach. The project is an
extended standalone port developed from **[salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b)**
(by `@i.erhan.es`), which in turn built upon the earlier Denon DJ **Prime GO** and **SC Live 4** (`JP21`)
research.

## What this repository contains

* Original shell scripts, C sources, shims (`knobshim.so`, `fbshim.so`, `audioshim.so`, `netshim.so`, `crashcatch.so`), diagnostic tools, and technical documentation written for this project. These are licensed MIT (see `LICENSE`).
* Device-survey and hardware architectural specifications for Amlogic S905X (Meson GXL) and Raspberry Pi 4B (BCM2711).
* Control surface mappings for the 4-channel **Pioneer DDJ-FLX6** (`map_flx6.c`), 2-channel **DDJ-FLX4** (`map_flx4.c`), Denon JP21 (`map_jp21.c`), and evdev keyboard fallback (`map_kbd.c`).
* Interoperability patch instructions for the `rbp` binary, DirectFB 1.4 multi-plane DRM integration, and Pro DJ Link networking stack.

## What this repository does **not** contain

* No XDJ-RX3/CDJ firmware (`.UPD`), no decrypted firmware ISO, no `rootfs`, no
  `rbp`/`rb` executable, and no other Pioneer/AlphaTheta binaries.
* No Denon DJ / Engine OS files or binaries.
* No Armbian OS or Raspberry Pi OS images, bootloaders, or proprietary blobs.
* No rekordbox music, playlists, analysis files, or proprietary databases.
* No firmware decryption keys of any kind, and no firmware acquisition or
  decryption tooling — those are out of scope here and handled by related
  projects.

You must own/obtain the hardware and the extracted firmware assets yourself.
The scripts here operate strictly on files **you** supply.

## Provenance and related work this project builds on

* **[salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b)** by `@i.erhan.es` — the initial Raspberry Pi 4B port baseline.
* The **PrimeBox** XDJ-RX3 → Prime GO (RK3288) port — the source of the `rbp`
  patcher (`patch-rbp`), the patched DirectFB fbdev driver, and shared shim sources.
* The **rb2go** XDJ-RX3 → postmarketOS phone (aarch64) port — window-mode,
  input, and USB-emulation notes.
* The **chromebit** XDJ-RX3 → ASUS Chromebit (RK3288, postmarketOS) port — a
  clean Linux RK3288 reference.
* **freelive4** — root access methodology on Denon OS hardware.

## Legal caveats

* Firmware acquisition and decryption are **not** part of this project; they
  may be restricted in your jurisdiction. Check your local law before using any
  external extraction tools.
* Patching and running a vendor application on third-party hardware may violate
  the vendor's EULA. This project is offered for research, repair,
  preservation, and personal interoperability only.
* Installing this on your device can brick it or void its warranty. **You do
  everything at your own risk.**

## Trademarks

*Pioneer DJ*, *AlphaTheta*, *rekordbox*, *XDJ-RX3*, *CDJ*, *DDJ-FLX4*, *DDJ-FLX6*, and related marks
are trademarks of AlphaTheta Corporation. *Amlogic* is a trademark of Amlogic, Inc.
*Raspberry Pi* and *Raspberry Pi OS* are trademarks of Raspberry Pi Ltd.
*Denon DJ*, *SC Live 4*, *Engine OS*, and *Prime GO* are trademarks of inMusic Brands, Inc.
*Armbian* is a trademark of the Armbian project. All trademarks are used here in a descriptive,
nominative sense only.

## Third-party components

| Component | License | Used for |
|---|---|---|
| DirectFB 1.4 | LGPL-2.1 | display stack (patched fbdev driver) |
| JUCE | GPL / commercial | audio + UI framework inside `rbp` |
| ALSA / alsa-lib | LGPL | audio |
| glibc 2.13 (RX3 rootfs) | LGPL | soft-float runtime |
| BusyBox | GPL-2.0 | runtime shell |

Raspberry Pi OS is the host system this project targets, not a component it
ships or links against, and no part of it is redistributed here. Staying on the
fbdev path — rather than moving to DirectFB's DRM/KMS system — is deliberate
partly because it introduces **no new third-party code**: the display path is
the same patched fbdev driver, presenting to a different framebuffer.

See each project for the full license text.
