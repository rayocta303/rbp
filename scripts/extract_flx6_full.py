import re

with open("/tmp/flx6_midi_layout.txt", "r", encoding="utf-8", errors="ignore") as f:
    lines = f.readlines()

out = []
current_section = "GENERAL"

for line in lines:
    stripped = line.strip()
    # Check section headers: "1. DECK", "2. MIXER", etc.
    m_sec = re.match(r"^(\d+\.\s+[A-Z\s]+)", stripped)
    if m_sec:
        current_section = m_sec.group(1).strip()
        out.append(f"\n### {current_section}\n")
        out.append("| UI Name | Trigger / Shift | Deck | Ch | Type | No. (Dec) | Data1 (Hex) | Details |")
        out.append("| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |")
        continue

    # Look for table rows with Note or CC
    # In pdftotext -layout, columns are roughly fixed width or spaced
    # Example:
    # PLAY/PAUSE L   No   Press   Deck 1   -   1   Note   11   B-1   0x90   0x0B   dd   ...
    # Look for Note / CC in the line
    m = re.search(r"\b(Deck\s+[1-4]|Master|-)?\s+(\d{1,2})\s+(Note|CC)\s+(\d{1,3})\s+([A-G#-0-9]+)?\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})\s+([0-9a-zA-Z]+)?\s*(.*?)$", stripped)
    if m:
        deck = m.group(1) or "-"
        ch = m.group(2)
        kind = m.group(3)
        num = m.group(4)
        status_hex = m.group(6)
        data1_hex = m.group(7)
        details = m.group(9) or ""
        
        # Left portion is UI name and Trigger
        left = stripped[:m.start()].strip()
        out.append(f"| {left} | - | {deck} | {ch} | {kind} | {num} | {data1_hex} | {details[:50]} |")

with open("/tmp/ddj_flx6_complete_midi_map.md", "w", encoding="utf-8") as f:
    f.write("\n".join(out))

print(f"Extracted {len(out)} lines to /tmp/ddj_flx6_complete_midi_map.md")
