import re

with open("/tmp/flx6_midi_layout.txt", "r", encoding="utf-8", errors="ignore") as f:
    text = f.read()

# Let's inspect unique UI names and their mappings
lines = text.split("\n")

current_group = ""
current_ui = ""
entries = []

for line in lines:
    m_grp = re.match(r"^\s*(\d+\.\s+[A-Z\s]+)", line)
    if m_grp:
        current_group = m_grp.group(1).strip()
    
    # Check if line contains MIDI Channel and Note/CC
    # Look for "Note" or "CC" followed by numbers and hex 0x..
    m_midi = re.search(r"(\bDeck\s+\d|\bMaster|\b-\b)?\s+(\d{1,2})\s+(Note|CC)\s+(\d{1,3})\s+([A-G#-0-9]+)?\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})", line)
    if m_midi:
        deck_str = m_midi.group(1) or ""
        ch = m_midi.group(2)
        kind = m_midi.group(3)
        num = m_midi.group(4)
        note_name = m_midi.group(5) or ""
        status_hex = m_midi.group(6)
        data1_hex = m_midi.group(7)
        
        # Look for UI name at start of line
        prefix = line[:m_midi.start()].strip()
        entries.append((current_group, prefix, deck_str, ch, kind, num, status_hex, data1_hex, line.strip()))

print(f"Total extracted MIDI mappings: {len(entries)}")
# Print samples from each section
groups = {}
for e in entries:
    groups.setdefault(e[0], []).append(e)

for g, items in groups.items():
    print(f"\n=== GROUP: {g} ({len(items)} items) ===")
    for item in items[:15]:
        print(f"  {item[1]:<30} | {item[2]:<8} | Ch {item[3]:<2} | {item[4]} {item[5]:<3} ({item[7]})")
