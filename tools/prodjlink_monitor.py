#!/usr/bin/env python3
"""
Pro DJ Link Real-Time TUI Traffic Monitor for Windows
====================================================
Fixed-layout, flicker-free terminal dashboard for monitoring Pioneer
Pro DJ Link traffic on UDP ports 50000, 50001, and 50002.
"""

import socket
import struct
import sys
import os
import time
import threading
import argparse
from collections import deque

try:
    import msvcrt
    HAS_MSVCRT = True
except ImportError:
    HAS_MSVCRT = False

# Ensure UTF-8 stdout encoding on Windows
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

# ANSI Color and Formatting Codes
ESC = "\033["
RESET = f"{ESC}0m"
BOLD = f"{ESC}1m"
DIM = f"{ESC}2m"
GREEN = f"{ESC}92m"
YELLOW = f"{ESC}93m"
CYAN = f"{ESC}96m"
MAGENTA = f"{ESC}95m"
RED = f"{ESC}91m"
WHITE = f"{ESC}97m"
BG_BLUE = f"{ESC}44m"
BG_GRAY = f"{ESC}100m"

# Cursor Controls
HIDE_CURSOR = f"{ESC}?25l"
SHOW_CURSOR = f"{ESC}?25h"
CURSOR_HOME = f"{ESC}H"
CLEAR_SCREEN = f"{ESC}2J"
CLEAR_EOL = f"{ESC}K"

WIDTH = 88
SEP_LINE = "=" * WIDTH
THIN_LINE = "-" * WIDTH


MAGIC_HEADERS = (
    b"Qspt1WmJOL",
    b"Qsp1WmJO2",
    b"Qspt1WmJO2",
    b"Qspt1WmJO3",
    b"PioneerPRODJ",
)

PACKET_TYPES = {
    0x00: "Discovery Query",
    0x01: "Announce Reply",
    0x02: "Change Request",
    0x03: "Status / Config",
    0x04: "Channel Claim",
    0x06: "Device Keepalive",
    0x08: "Drop / Leave",
    0x0a: "Initial Announce",
    0x20: "Player Status",
    0x28: "Beat / Tempo Sync",
    0x2c: "Mixer Status",
    0x32: "Extended Status",
}

active_devices = {}
device_lock = threading.Lock()
recent_events = deque(maxlen=8)
events_lock = threading.Lock()
total_packets = 0
last_probe_status = "Ready"
running = True


def get_local_interfaces():
    """Retrieve available IPv4 addresses on Windows."""
    ips = []
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except Exception:
        pass

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        default_ip = s.getsockname()[0]
        s.close()
        if default_ip not in ips and not default_ip.startswith("127."):
            ips.append(default_ip)
    except Exception:
        pass

    if "192.168.137.1" not in ips:
        try:
            test_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            test_sock.bind(("192.168.137.1", 0))
            test_sock.close()
            ips.append("192.168.137.1")
        except Exception:
            pass

    return ips


def format_mac(raw_bytes):
    return ":".join(f"{b:02x}" for b in raw_bytes)


def decode_prodjlink_packet(data, src_ip, src_port, dst_port):
    """Parse Pro DJ Link binary packet."""
    global total_packets
    if len(data) < 10:
        return None

    magic = None
    magic_len = 0
    for h in MAGIC_HEADERS:
        if data.startswith(h):
            magic = h
            magic_len = len(h)
            break

    if not magic:
        if data[:4] in (b"Qspt", b"Qsp1"):
            magic = data[:10]
            magic_len = 10
        else:
            return None

    total_packets += 1
    pkt_type = data[magic_len] if len(data) > magic_len else 0
    type_name = PACKET_TYPES.get(pkt_type, f"Type 0x{pkt_type:02x}")

    info = {
        "type": pkt_type,
        "type_name": type_name,
        "device_name": None,
        "device_num": None,
        "ip": src_ip,
        "mac": None,
        "bpm": None,
        "raw_len": len(data),
    }

    if pkt_type in (0x01, 0x06, 0x0a) and len(data) >= 0x30:
        name_raw = data[0x0C:0x24]
        dev_name = name_raw.split(b"\x00")[0].decode("utf-8", errors="ignore").strip()
        dev_num = data[0x24]
        mac_raw = data[0x26:0x2C]
        ip_raw = data[0x2C:0x30]

        ip_str = socket.inet_ntoa(ip_raw) if len(ip_raw) == 4 else src_ip
        mac_str = format_mac(mac_raw) if len(mac_raw) == 6 else "Unknown"

        info["device_name"] = dev_name or "Unknown"
        info["device_num"] = dev_num
        info["ip"] = ip_str
        info["mac"] = mac_str

        with device_lock:
            key = f"{ip_str}:{dev_num}"
            now = time.time()
            if key not in active_devices:
                active_devices[key] = {
                    "name": dev_name or "Unknown",
                    "player": dev_num,
                    "ip": ip_str,
                    "mac": mac_str,
                    "first_seen": now,
                    "last_seen": now,
                    "packets": 1,
                    "last_type": type_name,
                }
            else:
                d = active_devices[key]
                d["last_seen"] = now
                d["packets"] += 1
                d["last_type"] = type_name
                if dev_name:
                    d["name"] = dev_name

    elif pkt_type == 0x28 and len(data) >= 0x60:
        info["device_num"] = data[0x24] if len(data) > 0x24 else None
        try:
            bpm_raw = struct.unpack("!I", data[0x54:0x58])[0]
            if 3000 <= bpm_raw <= 30000:
                info["bpm"] = bpm_raw / 100.0
        except Exception:
            pass

    # Push to recent events ring buffer
    ts = time.strftime("%H:%M:%S")
    dev_label = f"{info['device_name'] or 'Peer'}"
    if info['device_num']:
        dev_label += f" #{info['device_num']}"

    bpm_suffix = f" (BPM: {info['bpm']:.1f})" if info['bpm'] else ""
    event_line = f"{ts} | UDP {dst_port} | {src_ip:<15} -> [{dev_label:<14}] {type_name}{bpm_suffix} ({len(data)}B)"

    with events_lock:
        recent_events.append(event_line)

    return info


def send_announce_probe(bind_ip):
    """Broadcast an Announce Query to wake up Pro DJ Link peers."""
    global last_probe_status
    try:
        probe_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

        packet = bytearray([
            0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c,
            0x00, 0x00, 0x00,
        ])
        packet += b"PRO_LINK_MONITOR".ljust(24, b"\x00")
        packet += b"\x00" * (50 - len(packet))

        targets = ["255.255.255.255"]
        if bind_ip.startswith("192.168.137."):
            targets.append("192.168.137.255")
            targets.append("192.168.137.227")

        for tgt in targets:
            try:
                probe_sock.sendto(packet, (tgt, 50000))
            except Exception:
                pass

        probe_sock.close()
        last_probe_status = f"Sent at {time.strftime('%H:%M:%S')}"
    except Exception as e:
        last_probe_status = f"Err: {str(e)[:20]}"


def udp_listener(port, bind_ip):
    """Worker thread listening on specific UDP port."""
    global running
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((bind_ip, port))
    except Exception as e:
        with events_lock:
            recent_events.append(f"BIND ERROR on port {port}: {e}")
        return

    sock.settimeout(0.5)
    while running:
        try:
            data, addr = sock.recvfrom(4096)
            src_ip, src_port = addr
            decode_prodjlink_packet(data, src_ip, src_port, port)
        except socket.timeout:
            continue
        except Exception:
            if not running:
                break

    sock.close()


def render_tui(bind_ip, ports_str):
    """Render the fixed layout dashboard."""
    now = time.time()
    timestr = time.strftime("%H:%M:%S")

    lines = []

    # Title Bar
    lines.append(f"{BOLD}{BG_BLUE}{WHITE}{' PIONEER PRO DJ LINK - REALTIME MONITOR':<{WIDTH}}{RESET}")
    info_line = f" Interface: {bind_ip}  |  Ports: {ports_str}  |  Time: {timestr}"
    lines.append(f"{DIM}{info_line:<{WIDTH}}{RESET}")
    keys_line = f" Hotkeys: [P] Send Probe   [C] Clear Events   [Q] Quit"
    lines.append(f"{CYAN}{keys_line:<{WIDTH}}{RESET}")
    lines.append("=" * WIDTH)

    # Active Devices Section
    with device_lock:
        dev_count = len(active_devices)
        lines.append(f"{BOLD}{YELLOW}-- ACTIVE PRO DJ LINK PEERS ({dev_count}) " + "-" * (WIDTH - 30) + f"{RESET}")
        lines.append(f"{BOLD}{'Player':<10} {'Device Name':<16} {'IP Address':<16} {'MAC Address':<18} {'Packets':<9} {'Status':<10} {'Age'}{RESET}")
        lines.append(THIN_LINE)

        if not active_devices:
            lines.append(f"{DIM}  Waiting for Pro DJ Link packets... (Press 'P' to send discovery probe){RESET}")
            # Pad empty lines to prevent shifting
            for _ in range(4):
                lines.append("")
        else:
            sorted_devs = sorted(active_devices.items(), key=lambda x: x[1].get("player", 0))
            count = 0
            for key, dev in sorted_devs:
                count += 1
                age = now - dev["last_seen"]
                if age < 4.0:
                    status_str = f"{GREEN}ONLINE{RESET}"
                else:
                    status_str = f"{RED}TIMEOUT{RESET}"

                p_str = f"Player {dev['player']}" if dev['player'] else "Host"
                age_str = f"{age:.1f}s"
                pkt_str = f"{dev['packets']:,}"
                line = f" {p_str:<9} {dev['name']:<16} {dev['ip']:<16} {dev['mac']:<18} {pkt_str:<9} {status_str:<18} {age_str}"
                lines.append(line)

            # Pad up to 5 rows so the box below doesn't jump
            for _ in range(max(0, 5 - count)):
                lines.append("")

        lines.append(THIN_LINE)

    # Live Packet Stream Section
    lines.append(f"{BOLD}{YELLOW}-- LIVE PACKET STREAM (Latest Events) " + "-" * (WIDTH - 38) + f"{RESET}")
    with events_lock:
        ev_list = list(recent_events)
        for i in range(8):
            if i < len(ev_list):
                lines.append(f" {ev_list[i]}"[:WIDTH])
            else:
                lines.append("")

    # Status Bar
    lines.append(SEP_LINE)
    status_bar = f" Total Packets: {total_packets:,}  |  Last Probe: {last_probe_status}  |  Active: Yes"
    lines.append(f"{BOLD}{WHITE}{BG_GRAY}{status_bar:<{WIDTH}}{RESET}")

    # Build final screen frame with CLEAR_EOL on every line
    frame = CURSOR_HOME + "\n".join(f"{line}{CLEAR_EOL}" for line in lines)
    sys.stdout.write(frame)
    sys.stdout.flush()


def main():
    global running
    parser = argparse.ArgumentParser(description="Pro DJ Link Real-Time TUI Traffic Monitor")
    parser.add_argument("-i", "--ip", help="Local interface IP to bind to (e.g. 192.168.137.1)")
    parser.add_argument("--probe", action="store_true", help="Send discovery probe on startup")
    parser.add_argument("-p", "--ports", default="50000,50001,50002", help="Comma-separated UDP ports")
    args = parser.parse_args()

    # Enable ANSI escape processing on Windows console
    if os.name == "nt":
        os.system("")

    local_ips = get_local_interfaces()
    selected_ip = args.ip

    if not selected_ip:
        if "192.168.137.1" in local_ips:
            selected_ip = "192.168.137.1"
        elif local_ips:
            selected_ip = local_ips[0]
        else:
            selected_ip = "0.0.0.0"

    ports = [int(p.strip()) for p in args.ports.split(",") if p.strip().isdigit()]

    # Clear screen once at startup and hide cursor
    sys.stdout.write(CLEAR_SCREEN + HIDE_CURSOR)
    sys.stdout.flush()

    # Start listener threads
    for port in ports:
        t = threading.Thread(target=udp_listener, args=(port, selected_ip), daemon=True)
        t.start()

    if args.probe:
        send_announce_probe(selected_ip)

    try:
        while running:
            # Check keyboard input (non-blocking)
            if HAS_MSVCRT and msvcrt.kbhit():
                ch = msvcrt.getch()
                # Handle special keys / arrows gracefully
                if ch in (b"\x00", b"\xe0"):
                    msvcrt.getch()
                else:
                    key = ch.decode("utf-8", errors="ignore").lower()
                    if key == "q":
                        running = False
                        break
                    elif key == "p":
                        send_announce_probe(selected_ip)
                    elif key == "c":
                        with events_lock:
                            recent_events.clear()

            # Render TUI frame
            render_tui(selected_ip, args.ports)
            time.sleep(0.15)  # ~6.6 FPS smooth, flicker-free refresh

    except KeyboardInterrupt:
        running = False
    finally:
        # Restore terminal cursor and clear
        sys.stdout.write(SHOW_CURSOR + "\n\n")
        sys.stdout.flush()
        print("Monitor stopped cleanly.")


if __name__ == "__main__":
    main()
