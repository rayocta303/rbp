import subprocess
from PIL import Image

# Capture raw fb0 from target
ssh_cmd = ["ssh", "root@192.168.137.22", "cat /dev/fb0"]
p = subprocess.Popen(ssh_cmd, stdout=subprocess.PIPE)
raw_bytes, _ = p.communicate()

print(f"Read {len(raw_bytes)} bytes from fb0")
if len(raw_bytes) >= 1280 * 800 * 4:
    # 32bpp BGRA or RGBA
    img = Image.frombytes("RGBA", (1280, 800), raw_bytes[:1280 * 800 * 4], "raw", "BGRA")
    out_path = r"C:\Users\UNXCHR\.gemini\antigravity\brain\db0be7d6-8d66-41d7-8d7e-4e19dfe498b2\screen_mods_active.png"
    img.save(out_path)
    print("Saved screenshot to", out_path)
