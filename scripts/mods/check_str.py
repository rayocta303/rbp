with open('/opt/rblive4/mods/prebuilt/librx3_core.so', 'rb') as f:
    for base, off in [
        (0x16b14, 0xfffeaea0),
        (0x16b2c, 0xfffed0c4),
        (0x16b74, 0xfffeb958),
        (0x16b80, 0xfffeac4b),
        (0x16b8c, 0xfffeb3f6),
        (0x16c00, 0xfffed269),
    ]:
        vaddr = (base + 8 + off) % 0x100000000 # in ARM PC is address + 8
        foff = vaddr # .rodata addr 0x1318 has offset 0x1318!
        f.seek(foff)
        s = f.read(64).split(b'\0')[0].decode('latin1', 'ignore')
        print(f"[{hex(base)}] -> vaddr {hex(vaddr)}: {s}")
