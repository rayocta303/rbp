from PIL import Image

with open('/dev/fb0', 'rb') as f:
    raw = f.read(1280 * 800 * 4)

img = Image.frombytes('RGBA', (1280, 800), raw, 'raw', 'BGRA')
img.save('/tmp/screen.png')
print("Captured /tmp/screen.png (1280x800 RGBA)")
