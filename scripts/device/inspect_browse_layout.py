#!/usr/bin/env python3
import struct
import subprocess
import sys

def get_symbols(path):
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

def get_nearest_symbol(sorted_syms, vma):
    best = None
    for s_addr, s_name in sorted_syms:
        if s_addr <= vma:
            best = (s_addr, s_name, vma - s_addr)
        else:
            break
    return best

def find_bl_callers(data, target_vma, sorted_syms):
    text_end_offset = 0x502c1c
    callers = []
    for offset in range(0, text_end_offset, 4):
        insn = struct.unpack('<I', data[offset:offset+4])[0]
        if (insn & 0x0f000000) == 0x0b000000:
            imm24 = insn & 0x00ffffff
            if imm24 & 0x00800000:
                imm24 -= 0x01000000
            pc = offset + 0x8000
            dest = pc + 8 + (imm24 << 2)
            if dest == target_vma:
                near = get_nearest_symbol(sorted_syms, pc)
                callers.append((offset, pc, near))
    return callers

def main():
    elf_path = '/opt/rblive4/rbx3-run/root/pdj/rbp'
    syms = get_symbols(elf_path)
    sorted_syms = sorted(syms.items(), key=lambda x: x[0])

    with open(elf_path, 'rb') as f:
        data = f.read()

    targets = [
        (0x0017fc48, 'CmnFunc_CmnInfo_GetUtilityTrackInfomationType'),
        (0x0017fc5c, 'CmnFunc_CmnInfo_GetUtilityPlayListView'),
        (0x0010069c, 'BrowseListManage'),
        (0x00112d3c, 'IsTrackList'),
    ]

    for vma, name in targets:
        callers = find_bl_callers(data, vma, sorted_syms)
        print(f"\n=======================================================")
        print(f"Target: {name} (0x{vma:08x}) called by {len(callers)} sites:")
        print(f"=======================================================")
        for off, pc, near in callers:
            fn = near[1] if near else "unknown"
            delta = f"+0x{near[2]:x}" if near else ""
            print(f"  PC 0x{pc:08x} inside: {fn} {delta}")

if __name__ == '__main__':
    main()
