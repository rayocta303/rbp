# Notice, Copyright and Legal — RBP-Amlogic-S905X

**RBP-Amlogic-S905X** is an independent interoperability, hardware-enablement, and digital preservation project.
It is **not affiliated with, endorsed by, or sponsored by** Pioneer DJ, AlphaTheta Corporation, Amlogic, Inc., Raspberry Pi Ltd, Denon DJ, inMusic Brands, or any of their subsidiaries.

The primary mission of this project is running the standalone Pioneer DJ XDJ-RX3 rekordbox player (`rbp`, internally called `rb`) on low-cost **Amlogic S905X-series TV Boxes and Single Board Computers (SBCs)** running **Armbian aarch64** (Linux 5.15+), utilizing the kernel's native **32-bit EL0 Support** (`CONFIG_COMPAT`), a soft-float ARM32 chroot, and userland hardware translation shims.

---

## Provenance and Credits

This project is an independent standalone repository and is **not a fork** of upstream, but proudly credits and builds upon the foundational research and open-source contributions of the following authors and projects:

1. **Base Shims & rbpi4b Architecture:**
   - **Repository:** [https://github.com/salesforcedoc/rbpi4b](https://github.com/salesforcedoc/rbpi4b)
   - **Author:** `@salesforcedoc` / `@i.erhan.es` ([Instagram](https://instagram.com/i.erhan.es))
   - *Credit:* Provided the original proof-of-concept userland shims (`knobshim`, `fbshim`, `audioshim`, `crashcatch`), DirectFB 1.4 fbdev patches, and ALSA / fake tsc2007 touch translation pipeline.

2. **XDJ-RX3 Mod Suite & Enhancements:**
   - **Creator:** `@dj_ventotto` ([Instagram: @dj_ventotto](https://www.instagram.com/dj_ventotto/))
   - *Credit:* Provided the advanced runtime performance mods for XDJ-RX3 (`librx3_core.so`, Stems separation, Keyshift & Key-Match, White Bright Theme, Custom BeatJump, Latin Search, Asshole Mode, and custom pad engines).

3. **Prior Reverse Engineering and Interoperability Efforts:**
   - **PrimeBox:** XDJ-RX3 on Rockchip RK3288 (Prime GO) — original `patch-rbp` and DirectFB display adaptation.
   - **rb2go:** XDJ-RX3 on postmarketOS aarch64 mobile devices — window-mode and input discovery.
   - **chromebit:** XDJ-RX3 on ASUS Chromebit (RK3288) — mainline Linux graphics validation.
   - **freelive4:** Root access methodology and architecture documentation.

---

## What this repository contains

* Original and adapted C sources, shims (`fbshim.so`, `knobshim.so`, `audioshim.so`, `netshim.so`, `crashcatch.so`), shell scripts, and systemd units specifically tuned for Amlogic S905X TV boxes.
* Native DirectFB 1.4 multi-plane DRM / fbdev integration and zero-copy VNC video streaming.
* Control surface mappings for 4-channel **Pioneer DDJ-FLX6** (`map_flx6.c`) and 2-channel **DDJ-FLX4** (`map_flx4.c`).
* Native RBP UI overlays: Top-bar mod quick-select buttons (`KEY`, `STEMS`, `RMX`, `MODS`), anti-aliased Wi-Fi / Bluetooth status indicator, and in-place SHORTCUT Dark/Light Theme toggle card.
* Technical architecture documentation, data dictionaries, and bring-up manuals for Amlogic S905X SBCs and TV boxes.

## What this repository does NOT contain

* **No proprietary Pioneer DJ or AlphaTheta firmware binaries**, no `.UPD` files, no decrypted ISOs, no `rootfs`, and no `rbp` executable.
* **No firmware decryption keys**, bypass tools, or extraction scripts.
* **No copyrighted audio files, rekordbox databases, analysis files, or playlists.**
* **No proprietary OS distribution images or bootloader blobs.**

You must own the relevant hardware and provide your own legally obtained and extracted firmware assets. All scripts in this repository operate strictly on files supplied by the user.

---

## Legal Caveats

* This software is intended strictly for educational, scientific research, hardware repair, personal interoperability, and digital preservation purposes.
* Executing vendor software on non-vendor hardware may be subject to software licensing terms and regional copyright exceptions (such as reverse engineering for interoperability). Users are solely responsible for ensuring compliance with local laws.
* Modifying firmware, flashing custom bootloaders, or running experimental software on commercial hardware carries inherent risks. The authors and contributors assume no liability for damaged hardware, data loss, or voided warranties.

---

## Trademarks

*Pioneer DJ*, *AlphaTheta*, *rekordbox*, *XDJ-RX3*, *DDJ-FLX6*, *DDJ-FLX4*, *CDJ*, and associated logos are registered trademarks of AlphaTheta Corporation.  
*Amlogic* is a registered trademark of Amlogic, Inc.  
*Raspberry Pi* is a trademark of Raspberry Pi Ltd.  
*Denon DJ*, *Engine OS*, and *Prime GO* are trademarks of inMusic Brands, Inc.  
*Armbian* is a trademark of the Armbian Project.  
All brand names, trademarks, and registered trademarks cited in this documentation are used strictly for identification and nominative reference purposes only.
