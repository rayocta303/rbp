import ctypes, ctypes.util, os, subprocess, time

libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
libc.mprotect.restype = ctypes.c_int
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

env = dict(os.environ)
env["PATH"] = "/bin:/sbin:/usr/bin:/usr/sbin"
env["LD_PRELOAD"] = "/usr/lib/crashcatch.so:/usr/lib/netshim.so:/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so"

cmd = ["chroot", "/opt/rblive4/rbx3-run", "/root/pdj/rbp", "-a"]
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(1)

pid = proc.pid
print(f"Testing write to pid {pid}")

try:
    with open(f"/proc/{pid}/mem", "r+b", 0) as f:
        f.seek(0x001d3284)
        cur = f.read(8)
        print("Current bytes at 0x001d3284:", cur.hex())
        # Try writing through /proc/$pid/mem
        f.seek(0x001d3284)
        f.write(b"\x00\x00\xa0\xe1\x00\x00\xa0\xe1")
        f.seek(0x001d3284)
        new = f.read(8)
        print("After write via /proc/mem:", new.hex())
        # Restore
        f.seek(0x001d3284)
        f.write(cur)
        print("Restored.")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

