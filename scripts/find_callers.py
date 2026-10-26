import struct

with open('/opt/rblive4/rbx3-run/root/pdj/rbp', 'rb') as f:
    data = f.read()

def find_bl(target_va):
    results = []
    base_va = 0x8000
    for offset in range(0, len(data) - 4, 4):
        pc = base_va + offset
        word = struct.unpack('<I', data[offset:offset+4])[0]
        if (word & 0xFF000000) == 0xEB000000:
            imm24 = word & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            dest = pc + 8 + (imm24 << 2)
            if dest == target_va:
                results.append(pc)
    return results

print("ui::Player::Player (0x2f7e90):", [hex(x) for x in find_bl(0x2f7e90)])
print("createPlayer (0x604dc):", [hex(x) for x in find_bl(0x604dc)])
