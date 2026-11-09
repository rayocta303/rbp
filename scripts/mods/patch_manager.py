#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
XDJ-RX3 Binary Patch Manager for RBP
Safely applies or reverts guarded binary patches to /opt/rblive4/rbx3-run/root/pdj/rbp.
Creates and preserves /opt/rblive4/rbx3-run/root/pdj/rbp.stock.
"""

import os
import sys
import shutil
import hashlib

# Patch definition: (offset, stock_bytes, patched_bytes, name)
PATCHES = {
    # 1. CORE: NS_GetImageInfoByID image ID table upper bound 0x15cc -> 0x16a5
    "core": [
        (1874220, bytes([0xCC, 0x35, 0x01, 0xE3]), bytes([0xA5, 0x36, 0x01, 0xE3]), "image-table-private-ids"),
    ],
    # 2. BEATJUMP 32BARS: 12 patches for pads 7/8 (-32/+32 bars)
    "beatjump_32bars": [
        (393376, bytes([0xC8, 0x8A, 0xB7, 0xEE]), bytes([0xC8, 0x8A, 0xB0, 0xEE]), "guard_vcvt_f64_to_vabs_f32"),
        (393380, bytes([0x08, 0x8B, 0x38, 0xEE]), bytes([0x08, 0x7A, 0x38, 0xEE]), "guard_vadd_f64_to_vadd_f32"),
        (393384, bytes([0xC8, 0x7B, 0xFC, 0xEE]), bytes([0xE7, 0x7A, 0xFC, 0xEE]), "guard_vcvt_u32_f64_to_f32"),
        (393600, bytes([0x00, 0x8A, 0xB2, 0xEE]), bytes([0xC2, 0x54, 0xA0, 0xE3]), "negative_jump_-32.0f"),
        (393604, bytes([0xC1, 0x54, 0xA0, 0xE3]), bytes([0x10, 0x5A, 0x08, 0xEE]), "negative_guard_32"),
        (393612, bytes([0x00, 0x8A, 0xB2, 0xEE]), bytes([0x42, 0x54, 0xA0, 0xE3]), "positive_jump_+32.0f"),
        (393616, bytes([0x10, 0x5A, 0x18, 0xEE]), bytes([0x10, 0x5A, 0x08, 0xEE]), "positive_guard_32"),
        (5016772, bytes([0x10, 0x00, 0x00, 0x00]), bytes([0x40, 0x00, 0x00, 0x00]), "LED_threshold_16_to_64_half-beats"),
        (5367248, bytes([0xF2, 0x13, 0x00, 0x00]), bytes([0x03, 0x14, 0x00, 0x00]), "PAD7_off_image_5106_to_5123"),
        (5367224, bytes([0xF3, 0x13, 0x00, 0x00]), bytes([0x04, 0x14, 0x00, 0x00]), "PAD7_on_image_5107_to_5124"),
        (5367188, bytes([0xF4, 0x13, 0x00, 0x00]), bytes([0x03, 0x14, 0x00, 0x00]), "PAD8_off_image_5108_to_5123"),
        (5367164, bytes([0xF5, 0x13, 0x00, 0x00]), bytes([0x04, 0x14, 0x00, 0x00]), "PAD8_on_image_5109_to_5124"),
    ],
    # 3. BEATJUMP NO QUANTIZE: Direct instant beat jump execution
    "beatjump_no_quantize": [
        (695764, bytes([0x3D, 0x00, 0x00, 0x0A]), bytes([0x00, 0x00, 0xA0, 0xE1]), "quantized_BeatJump_to_direct_path"),
    ],
}

DEFAULT_RBP_PATH = "/opt/rblive4/rbx3-run/root/pdj/rbp"

def get_rbp_path():
    if len(sys.argv) > 2 and sys.argv[2].startswith("/"):
        return sys.argv[2]
    return os.environ.get("RBP_PATH", DEFAULT_RBP_PATH)

def check_status(rbp_path):
    if not os.path.isfile(rbp_path):
        print(f"Error: {rbp_path} not found.")
        return 1

    with open(rbp_path, "rb") as f:
        print(f"Checking {rbp_path} ({os.path.getsize(rbp_path)} bytes):")
        for group, patch_list in PATCHES.items():
            print(f"\n--- Group: {group} ---")
            for off, stock, patched, name in patch_list:
                f.seek(off)
                data = f.read(len(stock))
                if data == stock:
                    st = "STOCK"
                elif data == patched:
                    st = "PATCHED"
                else:
                    st = f"UNKNOWN ({data.hex()})"
                print(f"  [{off}] {name:<35}: {st}")
    return 0

def apply_group(rbp_path, group_name):
    if group_name not in PATCHES:
        print(f"Unknown patch group: {group_name}. Available: {list(PATCHES.keys())}")
        return 1

    stock_backup = rbp_path + ".stock"
    if not os.path.isfile(stock_backup):
        print(f"Creating stock backup: {stock_backup}")
        shutil.copy2(rbp_path, stock_backup)

    with open(rbp_path, "r+b") as f:
        for off, stock, patched, name in PATCHES[group_name]:
            f.seek(off)
            cur = f.read(len(stock))
            if cur == patched:
                print(f"  [{off}] {name}: already PATCHED.")
            elif cur == stock:
                f.seek(off)
                f.write(patched)
                print(f"  [{off}] {name}: successfully PATCHED.")
            else:
                print(f"  [{off}] {name}: ERROR - bytes mismatch ({cur.hex()} != stock {stock.hex()})")
                return 1
    return 0

def revert_group(rbp_path, group_name):
    if group_name not in PATCHES:
        print(f"Unknown patch group: {group_name}. Available: {list(PATCHES.keys())}")
        return 1

    with open(rbp_path, "r+b") as f:
        for off, stock, patched, name in PATCHES[group_name]:
            f.seek(off)
            cur = f.read(len(stock))
            if cur == stock:
                print(f"  [{off}] {name}: already STOCK.")
            elif cur == patched:
                f.seek(off)
                f.write(stock)
                print(f"  [{off}] {name}: successfully REVERTED to stock.")
            else:
                print(f"  [{off}] {name}: WARNING - unknown bytes ({cur.hex()}), restoring stock.")
                f.seek(off)
                f.write(stock)
    return 0

def revert_all(rbp_path):
    stock_backup = rbp_path + ".stock"
    if os.path.isfile(stock_backup):
        print(f"Restoring entire binary from stock backup: {stock_backup}")
        shutil.copy2(stock_backup, rbp_path)
        print("Restored.")
        return 0
    for g in PATCHES:
        revert_group(rbp_path, g)
    return 0

def apply_all(rbp_path):
    for g in PATCHES:
        print(f"\nApplying group: {g}")
        ret = apply_group(rbp_path, g)
        if ret != 0:
            return ret
    return 0

if __name__ == "__main__":
    rbp = get_rbp_path()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"

    if cmd == "status":
        sys.exit(check_status(rbp))
    elif cmd == "apply":
        target = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("/") else "all"
        if target == "all":
            sys.exit(apply_all(rbp))
        else:
            sys.exit(apply_group(rbp, target))
    elif cmd == "revert":
        target = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("/") else "all"
        if target == "all":
            sys.exit(revert_all(rbp))
        else:
            sys.exit(revert_group(rbp, target))
    else:
        print("Usage: patch_manager.py [status|apply [group]|revert [group]] [rbp_path]")
        sys.exit(1)

