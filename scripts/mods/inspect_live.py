import os, subprocess, time, sys

env = dict(os.environ)
env["PATH"] = "/bin:/sbin:/usr/bin:/usr/sbin"
env["LD_PRELOAD"] = "/usr/lib/crashcatch.so:/usr/lib/netshim.so:/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so"

cmd = ["chroot", "/opt/rblive4/rbx3-run", "/root/pdj/rbp", "-a"]
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
print("Started rbp pid:", proc.pid)

time.sleep(1)

try:
    with open(f"/proc/{proc.pid}/mem", "rb", 0) as f:
        for addr, name in [
            (0x001d192c, "IMAGE_INFO"),
            (0x001d23e4, "PAL_DRAW_TEXT"),
            (0x001d3284, "PAL_DRAW_IMAGE"),
            (0x002dc104, "SOLVE_TOUCH"),
        ]:
            f.seek(addr)
            data = f.read(8)
            print(f"[{name}] at {hex(addr)}: {data.hex()}")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

