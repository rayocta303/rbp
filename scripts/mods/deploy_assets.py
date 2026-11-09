#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
XDJ-RX3 Mod Asset Deployer
Deploys core shared object, RGB565 UI textures, and audio samples into target chroot.
"""

import os
import sys
import shutil

ASSET_MAP = [
    # Core
    ("core/assets/glyph-atlas-dark.rgb565", "rx3-glyph-atlas-dark.rgb565"),
    ("core/assets/glyph-atlas-light.rgb565", "rx3-glyph-atlas-light.rgb565"),
    ("core/assets/status-none-selected.rgb565", "rx3-status-none-selected.rgb565"),
    ("core/assets/status-none-selected-light.rgb565", "rx3-status-none-selected-light.rgb565"),
    # Keyshift
    ("keyshift/assets/key-selected.rgb565", "rx3-key-selected.rgb565"),
    ("keyshift/assets/key-selected-light.rgb565", "rx3-key-selected-light.rgb565"),
    ("keyshift/assets/none-selected.rgb565", "rx3-none-selected.rgb565"),
    ("keyshift/assets/none-selected-light.rgb565", "rx3-none-selected-light.rgb565"),
    ("keyshift/assets/single-key-none.rgb565", "rx3-single-key-none.rgb565"),
    ("keyshift/assets/single-key-none-light.rgb565", "rx3-single-key-none-light.rgb565"),
    ("keyshift/assets/single-key-selected.rgb565", "rx3-single-key-selected.rgb565"),
    ("keyshift/assets/single-key-selected-light.rgb565", "rx3-single-key-selected-light.rgb565"),
    # Stems
    ("stems/assets/stems-selected.rgb565", "rx3-stems-selected.rgb565"),
    ("stems/assets/stems-selected-light.rgb565", "rx3-stems-selected-light.rgb565"),
    ("stems/assets/single-stems-none.rgb565", "rx3-single-stems-none.rgb565"),
    ("stems/assets/single-stems-none-light.rgb565", "rx3-single-stems-none-light.rgb565"),
    ("stems/assets/single-stems-selected.rgb565", "rx3-single-stems-selected.rgb565"),
    ("stems/assets/single-stems-selected-light.rgb565", "rx3-single-stems-selected-light.rgb565"),
    # Samples
    ("samples/assets/samples-selected.rgb565", "rx3-samples-selected.rgb565"),
    ("samples/assets/samples-selected-light.rgb565", "rx3-samples-selected-light.rgb565"),
    ("samples/assets/samples-none-selected.rgb565", "rx3-samples-none-selected.rgb565"),
    ("samples/assets/samples-none-selected-light.rgb565", "rx3-samples-none-selected-light.rgb565"),
    ("samples/assets/samples-beatfx-selected.rgb565", "rx3-samples-beatfx-selected.rgb565"),
    ("samples/assets/samples-beatfx-selected-light.rgb565", "rx3-samples-beatfx-selected-light.rgb565"),
    # Prebuilt Core .so
    ("prebuilt/librx3_core.so", "librx3_core.so"),
]

ROLL_WAVS = [
    ("roll/clap-ventotto.wav", "clap.wav"),
    ("roll/hat-ventotto.wav", "hat.wav"),
    ("roll/kick-ventotto.wav", "kick.wav"),
    ("roll/snare-ventotto.wav", "snare.wav"),
]

def deploy(mods_dir, target_pdj_dir):
    print(f"Deploying mod assets from {mods_dir} -> {target_pdj_dir}")
    os.makedirs(target_pdj_dir, exist_ok=True)

    for src_rel, dst_name in ASSET_MAP:
        src_path = os.path.join(mods_dir, src_rel)
        dst_path = os.path.join(target_pdj_dir, dst_name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, dst_path)
            os.chmod(dst_path, 0o755 if dst_name.endswith(".so") else 0o644)
            print(f"  [OK] {dst_name} ({os.path.getsize(dst_path)} bytes)")
        else:
            print(f"  [MISSING] {src_path}")

    # Roll sounds
    roll_dst = os.path.join(target_pdj_dir, "roll")
    os.makedirs(roll_dst, exist_ok=True)
    for src_rel, dst_name in ROLL_WAVS:
        src_path = os.path.join(mods_dir, src_rel)
        dst_path = os.path.join(roll_dst, dst_name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, dst_path)
            os.chmod(dst_path, 0o644)
            print(f"  [OK] roll/{dst_name}")
        else:
            print(f"  [MISSING] {src_path}")

    # Stems directory link & placeholder
    stems_dir = "/tmp/rx3-stems"
    if not os.path.exists(stems_dir):
        try:
            os.makedirs(stems_dir, exist_ok=True)
        except Exception:
            pass

    print("Asset deployment complete.")

if __name__ == "__main__":
    mods = sys.argv[1] if len(sys.argv) > 1 else "."
    target = sys.argv[2] if len(sys.argv) > 2 else "/opt/rblive4/rbx3-run/root/pdj"
    deploy(mods, target)

