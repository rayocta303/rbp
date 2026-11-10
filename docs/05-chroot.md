# 05 — Soft-Float ARM32 Chroot on Amlogic S905X

The Pioneer XDJ-RX3 rekordbox engine (`rbp`) runs inside `/opt/rblive4/rbx3-run`, a soft-float glibc-2.13 ARM32 chroot environment.

This document details how the 64-bit Armbian Linux kernel on the Amlogic S905X Cortex-A53 processor executes this soft-float 32-bit userland at native bare-metal speed.

---

## 1. Execution Model: 32-bit EL0 Support

The Amlogic S905X processor features four 64-bit ARM Cortex-A53 cores implementing the **ARMv8-A** architecture. ARMv8-A processors include hardware architectural support for executing 32-bit ARM instructions (A32 and T32) at Exception Level 0 (user mode).

When Armbian's 64-bit (`aarch64`) kernel is compiled with:
```ini
CONFIG_COMPAT=y
```
the kernel directly services 32-bit system calls via the `compat_sys_*` vector table. 

### Why This Matters:
1. **Zero Emulation Overhead:** `rbp` does not run under QEMU or user-mode emulation; CPU instructions execute directly on physical silicon registers.
2. **Native Performance:** DSP audio filtering, pitch-shifting, and DirectFB 2D rasterization achieve full 1.5 GHz throughput.
3. **Clean Host Tooling:** The host operating system remains a modern 64-bit Debian Bookworm / Armbian environment, allowing standard 64-bit SSH, Python 3, NetworkManager, and diagnostic tools to run side-by-side with the 32-bit chroot.

---

## 2. Chroot Directory Structure

Extracted at `/opt/rblive4/rbx3-run` (~200–250 MB total):

```
/opt/rblive4/rbx3-run/
├── bin/                   # Soft-float busybox, sh, mount
├── dev/                   # Bound to host /dev (fb0, snd, input, etc.)
├── etc/                   # Pioneer configs, mtab -> /proc/mounts
├── lib/                   # Soft-float glibc 2.13 ld-linux.so.3, libc.so.6, libm.so.6
├── proc/                  # Bound to host /proc
├── root/
│   ├── gui/               # GUI asset databases (fontdata, imagedata, pset, system)
│   ├── pdj/
│   │   ├── rbp            # Patched XDJ-RX3 player executable (ARM32 soft-float ELF)
│   │   └── edb_streamd    # Pioneer Rekordbox DeviceSQL database engine
│   └── settings/          # Touch calibration and hardware profile data
├── sys/                   # Bound to host /sys
├── tmp/                   # Inter-process pipes (IPC) and state files
└── usr/
    ├── etc/directfbrc     # DirectFB runtime configuration
    └── lib/
        ├── directfb-1.4-6/# Recompiled DirectFB 1.4.16 core and fbdev system driver
        ├── fbshim.so      # Framebuffer & touch translation shim
        ├── knobshim.so    # MIDI controller translation shim
        ├── audioshim.so   # ALSA low-latency audio router
        ├── netshim.so     # Pro DJ Link interface rewriter
        ├── crashcatch.so  # POSIX signal guard
        └── librx3_core.so # Performance mods hook runtime by @dj_ventotto
```

---

## 3. Building the Chroot (Host)

The build script [`scripts/build-chroot.sh`](../scripts/build-chroot.sh) assembles the entire deployment archive:

```sh
RX3=/path/to/extracted/XDJRX3-rootfs DFB="$PWD/work/dfb" scripts/build-chroot.sh
```

### Steps Executed by the Builder:
1. Validates that the input directory contains the genuine soft-float `glibc-2.13` dynamic linker (`lib/ld-linux.so.3`).
2. Stages Pioneer GUI asset tables (`imagedata.dat`, `fontdata.dat`, `pset`, `system`).
3. Applies binary patches to `rbp` (disables vendor board checks and watchdog).
4. Compiles and installs all shims with soft-float EABI flags (`-mfloat-abi=soft -march=armv7-a`).
5. Copies `librx3_core.so` and mod assets into place.
6. Packages the output into `work/rbx3-s905x.tgz`.

---

## 4. Verification on S905X

Once deployed to `/opt/rblive4/rbx3-run`, test execution inside the chroot:
```sh
chroot /opt/rblive4/rbx3-run /bin/busybox uname -a
```
Expected output:
```
Linux unx.dj 5.15.201-ophub ... armv7l GNU/Linux
```
*(Notice that busybox inside the chroot sees `armv7l` personality while the host kernel runs `aarch64` — confirming full 32-bit EL0 compatibility).*
