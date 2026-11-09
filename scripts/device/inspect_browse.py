#!/usr/bin/env python3
import struct
import subprocess
import sys

def get_symbol_map(path):
    syms = {}
    out = subprocess.check_output(['nm', '-C', path], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] != 'U':
            try:
                addr = int(parts[0], 16)
                name = " ".join(parts[2:])
                syms[addr] = name
            except ValueError:
                pass
    return syms

def find_nearest_symbol(sym_list, vma):
    best = None
    for s_addr, s_name in sym_list:
        if s_addr <= vma:
            best = (s_addr, s_name, vma - s_addr)
        else:
            break
    return best

def main():
    elf_path = '/opt/rblive4/rbx3-run/root/pdj/rbp'
    print(f"Loading symbol map from {elf_path}...")
    syms = get_symbol_map(elf_path)
    sorted_syms = sorted(syms.items(), key=lambda x: x[0])

    with open(elf_path, 'rb') as f:
        data = f.read()

    targets = {
        0x02685b4c: 'ui::B011a_',
        0x02685b6c: 'ui::B011b_',
        0x02685b8c: 'ui::B011r_',
        0x0114c2d0: 'BrowseUiIf::_singletonInstance',
    }

    for addr, name in targets.items():
        pat = struct.pack('<I', addr)
        idx = 0
        matches = []
        while True:
            idx = data.find(pat, idx)
            if idx == -1:
                break
            matches.append(idx)
            idx += 4
        print(f"\n=======================================================")
        print(f"{name} (0x{addr:08x}) referenced at {len(matches)} locations:")
        print(f"=======================================================")
        for m in matches:
            if m < 0x502c1c:
                vma = m + 0x8000
            else:
                vma = m + 0x10000
            near = find_nearest_symbol(sorted_syms, vma)
            if near:
                print(f"  File offset 0x{m:06x} -> VMA 0x{vma:08x} inside: {near[1]} (+0x{near[2]:x})")
            else:
                print(f"  File offset 0x{m:06x} -> VMA 0x{vma:08x}")

if __name__ == '__main__':
    main()
