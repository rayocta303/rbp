import re

with open("/tmp/flx6_midi.txt", "r", encoding="utf-8", errors="ignore") as f:
    lines = [line.strip() for line in f]

for i, line in enumerate(lines):
    # Print lines that look like main categories or headings
    if any(k in line.upper() for k in ["MIXER", "BROWSE", "EFFECT", "PAD", "JOG", "TEMPO", "LOOP", "ILLUMINATION"]):
        if len(line) < 30 and ("." in line or line.isupper()):
            print(f"Line {i}: {line}")
