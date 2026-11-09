with open("/opt/rblive4/mods/prebuilt/librx3_core.so", "rb") as f:
    b = f.read()
print("1d23e4 (DrawText):", b.find(b"\xe4\x23\x1d\x00"))
print("1d3284 (DrawImage):", b.find(b"\x84\x32\x1d\x00"))
print("1d192c (ImageInfo):", b.find(b"\x2c\x19\x1d\x00"))
print("DrawText guard:", b.find(b"\xf0\x4f\x2d\xe9\x4d\xdf\x4d\xe2"))
print("DrawImage guard:", b.find(b"\xf0\x4f\x2d\xe9\xcc\xd0\x4d\xe2"))

