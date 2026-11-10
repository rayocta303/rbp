# 03 — Port Plan & Architecture Migration to Amlogic S905X

This document outlines the architectural reuse map, modifications, and enhancements implemented in **RBP-Amlogic-S905X** to bring the standalone Pioneer XDJ-RX3 rekordbox engine to Amlogic S905X hardware with 4-channel controller support and native performance mods.

---

## 1. Architectural Reuse & Adaptation Map

| Component | Source Baseline ([rbpi4b](https://github.com/salesforcedoc/rbpi4b)) | Adapted for RBP-Amlogic-S905X |
|---|---|---|
| **Binary Patcher** | `patch-rbp` (Python/C) | **Reused:** Disables Pioneer board checks and hardware watchdog. |
| **Runtime Chroot** | `scripts/build-chroot.sh` | **Adapted:** Assembles soft-float ARM32 environment for Armbian aarch64 on Cortex-A53 (`/opt/rblive4/rbx3-run`). |
| **Display Stack** | Patched DirectFB 1.4 fbdev | **Tuned for `meson-drm`:** Configured for native 1280×800 RGB565 HDMI output without rotation overhead. |
| **Shadow Framebuffer** | `fbshim.so` (single-buffer display) | **Enhanced:** Added clipping regions for active modals, eliminating 60Hz flickering on single-buffered `/dev/fb0`. |
| **Touch Translation** | Fake `tsc2007` emulation | **Enhanced:** Added multi-touch support for USB capacitive touch panels (`wch.cn USB2IIC_CTP_CONTROL`, Goodix). |
| **4-Channel Controller**| 2-channel DDJ-FLX4 only | **Added:** Dedicated 4-channel mapping for Pioneer **DDJ-FLX6** (`map_flx6.c`), 720 PPR jog, 6 FX racks, and Merge FX. |
| **DSP Anti-Glitch** | None (DSP clicks on channel switch) | **Added:** Automated Beat FX silence before channel switches to eliminate audio pops. |
| **Top Bar Mod Buttons**| Custom tabs above STATUS/BEAT FX | **Redesigned:** Moved `KEY`, `STEMS`, `RMX`, `MODS` to top header (`y: 8..44`), keeping ZOOM/GRID/STATUS/BEAT FX native. |
| **Theme Toggle Card** | Terminal sentinel toggle only | **Added:** Native in-place **THEME** card on SHORTCUT screen (`browse_mode == 10`) replacing the unused JOG LCD card. |
| **Performance Mods** | None (Stock RBP only) | **Integrated:** Support for `@dj_ventotto`'s `librx3_core.so` mod suite (Stems, Harmonic Mixing, Bright Theme). |

---

## 2. Key Enhancements Specific to Amlogic S905X

### 2.1 DirectFB Presentation over `meson-drm`
Amlogic's kernel DRM driver (`meson-drm`) exposes `/dev/fb0` as a DRM plane. The patched DirectFB driver communicates with this plane in single-buffered mode. To prevent visible tearing and flickering, `fbshim.so` intercepts `mmap` calls on `/dev/fb0` and introduces an in-memory shadow framebuffer (`g_shadow_fb`). During `FBIOPAN_DISPLAY` or `FBIO_WAITFORVSYNC`, the completed frame is blitted atomically to the hardware scanout buffer.

### 2.2 Relocating Mod Controls to the Top Header
In earlier implementations, custom mod tabs (such as Stems and Keyshift) were drawn above the native `STATUS` and `BEAT FX` buttons. This caused interference with native UI elements, partially obscuring the `ZOOM` and `GRID` buttons and waveform borders.

In **RBP-Amlogic-S905X**, mod navigation is relocated to the top bar (`x: 580..925, y: 8..44`):
* `KEY` (`x: 580..655`)
* `STEMS` (`x: 665..750`)
* `RMX` (`x: 760..835`)
* `MODS` (`x: 845..925`)
* `Wi-Fi / BT Strip` (`x: 939..1066`)

By keeping custom navigation in the top bar and disabling lower tab slots in `load-mods.sh`, the native Pioneer performance controls (**ZOOM, GRID, STATUS & BEAT FX**) remain 100% visible, responsive, and untouched.

### 2.3 SHORTCUT Screen Theme Toggle Integration
Rather than using key shortcuts or SSH commands to switch between Dark and Light mode, `ui_overlay.c` dynamically renders a native-styled **THEME** card directly inside the SHORTCUT view (`browse_mode == 10`):
* Overwrites the inactive **JOG LCD** section (`x: 792..1259, y: 89..198`).
* Matches the exact font, palette, amber top accent bar, and border style of adjacent Pioneer cards.
* Provides `[ DARK ]` and `[ LIGHT ]` segmented buttons that trigger runtime theme remapping via `theme-white`.
