# work/

Local build scratch directory for **RBP-Amlogic-S905X**, **gitignored** except for this file.

Use this directory to stage intermediate artifacts, builds, and source trees that should never be committed into the repository:

* `dfb-src/` — Pristine DirectFB 1.4.16 git tree used for applying `directfb-full.diff`.
* `flux-src/` — The `flux` package containing the `fluxcomp` IDL compiler required by DirectFB.
* `dfb/` — Staged DirectFB 1.4.16 soft-float installation output.
* `rbx3-s905x.tgz` — The compiled chroot bundle assembled by `scripts/build-chroot.sh`.
* Extracted XDJ-RX3 firmware directories (`XDJRX3-rootfs/`).
* Local device frame dumps (`.raw`, `.png`), core dumps, and profiling logs.

> [!CAUTION]
> Never place decrypted vendor firmware ISOs, decryption keys, or proprietary `rbp` binaries into public or tracked locations. Keep your workspace clean and secure.
