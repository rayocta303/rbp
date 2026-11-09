import ctypes, ctypes.util, os, subprocess, time

libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
libc.mprotect.restype = ctypes.c_int
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

# Test mprotect on rbp's code page directly in a running rbp
cmd = ["chroot", "/opt/rblive4/rbx3-run", "/root/pdj/rbp", "-a"]
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(1)

pid = proc.pid
print("Testing mprotect on pid", pid)

try:
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            if "001d3000" in line or "rbp" in line:
                print("Map:", line.strip())
finally:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

