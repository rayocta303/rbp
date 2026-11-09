import os, sys

pid = os.popen('pgrep -o rbp').read().strip()
if not pid:
    print("rbp not running")
    sys.exit(1)

with open(f"/proc/{pid}/mem", "rb", 0) as f:
    for addr, name in [(0x001d23e4, "PAL_DRAW_TEXT"), (0x001d3284, "PAL_DRAW_IMAGE"), (0x001d192c, "IMAGE_INFO"), (0x002dc104, "SOLVE_TOUCH")]:
        f.seek(addr)
        d = f.read(16)
        print(f"{name} at {hex(addr)}: {d.hex()}")

