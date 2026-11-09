import ctypes, ctypes.util
libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mprotect.restype = ctypes.c_int
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

p = libc.mmap(0, 4096, 3, 0x22, -1, 0)
print("mmap addr:", hex(p or 0))
r = libc.mprotect(p, 4096, 7) # PROT_READ | PROT_WRITE | PROT_EXEC
print("mprotect RWX ret:", r, "errno:", ctypes.get_errno())
r_rx = libc.mprotect(p, 4096, 5) # PROT_READ | PROT_EXEC
print("mprotect RX ret:", r_rx, "errno:", ctypes.get_errno())
r_rw = libc.mprotect(p, 4096, 3) # PROT_READ | PROT_WRITE
print("mprotect RW ret:", r_rw, "errno:", ctypes.get_errno())
