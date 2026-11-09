#!/usr/bin/env python3
"""
net_daemon.py -- Background OS Network & Wireless Daemon for XDJ-RX3.
Interfaces with Linux nmcli, rfkill, and ip to provide asynchronous network
state and handle UI control requests from the net_ui shim.

State file: /tmp/xdj_net_state.txt
Command FIFO: /tmp/xdj_net_cmd.fifo
"""

import os
import sys
import time
import subprocess
import select

STATE_FILE = "/tmp/xdj_net_state.txt"
CMD_FIFO = "/tmp/xdj_net_cmd.fifo"

def run_cmd(args, timeout=3):
    try:
        res = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return res.stdout.strip()
    except Exception:
        return ""

def get_wifi_radio():
    out = run_cmd(["nmcli", "radio", "wifi"])
    return 1 if "enabled" in out.lower() else 0

def get_bt_status():
    out = run_cmd(["rfkill", "list", "bluetooth"])
    if not out:
        return 0
    # If soft blocked is yes, return 0 (off), otherwise 1 (on)
    if "Soft blocked: yes" in out:
        return 0
    return 1

def get_ip(iface):
    out = run_cmd(["ip", "-4", "-o", "addr", "show", "dev", iface])
    # Format: 2: wlan0    inet 192.168.137.22/24 ...
    for part in out.split():
        if "/" in part and not part.startswith("inet"):
            return part.split("/")[0]
    return ""

def get_hostname():
    try:
        import socket
        return socket.gethostname() or "unx.dj"
    except Exception:
        return "unx.dj"

def get_wifi_list():
    """Returns (connected_ssid, items: list of (in_use, ssid, signal, security))"""
    out = run_cmd(["nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list"], timeout=4)
    items = []
    seen = set()
    connected_ssid = ""

    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(":")
        if len(parts) < 4:
            continue
        in_use_str = parts[0].strip()
        ssid = parts[1].strip()
        signal_str = parts[2].strip()
        security = parts[3].strip()

        if not ssid or ssid == "--":
            continue

        in_use = 1 if in_use_str == "*" else 0
        if in_use:
            connected_ssid = ssid

        try:
            signal = int(signal_str)
        except ValueError:
            signal = 50

        if ssid in seen:
            continue
        seen.add(ssid)

        items.append({
            "in_use": in_use,
            "ssid": ssid,
            "signal": signal,
            "security": security
        })

    # Sort so connected is first, then highest signal
    items.sort(key=lambda x: (x["in_use"], x["signal"]), reverse=True)
    return connected_ssid, items[:8]

def update_state(force_scan=False):
    wifi_radio = get_wifi_radio()
    bt_status = get_bt_status()
    wlan_ip = get_ip("wlan0")
    eth_ip = get_ip("eth0")
    hostname = get_hostname()

    connected_ssid = ""
    wifi_items = []

    if wifi_radio:
        connected_ssid, wifi_items = get_wifi_list()

    # Generate state string
    lines = [
        f"wifi_enabled={wifi_radio}",
        f"wifi_connected={1 if (wlan_ip and connected_ssid) else 0}",
        f"wifi_ssid={connected_ssid}",
        f"wifi_ip={wlan_ip}",
        f"bt_enabled={bt_status}",
        f"bt_name=XDJ-RX3-UNX",
        f"ap_enabled=0",
        f"ap_ssid=XDJ-RX3-DIRECT",
        f"ap_pass=xdjrx3wireless",
        f"ap_ip=192.168.42.1",
        f"ap_clients=0",
        f"eth_ip={eth_ip}",
        f"hostname={hostname}",
        f"net_count={len(wifi_items)}"
    ]

    for idx, it in enumerate(wifi_items):
        lines.append(f"net_{idx}={it['in_use']}:{it['ssid']}:{it['signal']}:{it['security']}")

    tmp_path = STATE_FILE + ".tmp"
    with open(tmp_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp_path, STATE_FILE)

def handle_cmd(cmd):
    cmd = cmd.strip()
    if not cmd:
        return
    print(f"[net_daemon] command received: {cmd}", flush=True)

    if cmd == "wifi_toggle":
        curr = get_wifi_radio()
        target = "off" if curr else "on"
        run_cmd(["nmcli", "radio", "wifi", target])
        time.sleep(0.5)
        update_state()

    elif cmd == "wifi_scan":
        run_cmd(["nmcli", "dev", "wifi", "rescan"])
        time.sleep(1.0)
        update_state()

    elif cmd.startswith("wifi_connect:"):
        ssid = cmd.split(":", 1)[1]
        print(f"[net_daemon] connecting to: {ssid}", flush=True)
        run_cmd(["nmcli", "dev", "wifi", "connect", ssid], timeout=15)
        update_state()

    elif cmd == "bt_toggle":
        curr = get_bt_status()
        if curr:
            run_cmd(["rfkill", "block", "bluetooth"])
        else:
            run_cmd(["rfkill", "unblock", "bluetooth"])
        time.sleep(0.3)
        update_state()

    elif cmd == "restart_net":
        run_cmd(["systemctl", "restart", "NetworkManager"])
        time.sleep(2.0)
        update_state()

def main():
    print("[net_daemon] Starting network daemon for XDJ-RX3...", flush=True)

    # Ensure FIFO exists
    if os.path.exists(CMD_FIFO):
        try:
            os.remove(CMD_FIFO)
        except Exception:
            pass
    try:
        os.mkfifo(CMD_FIFO, 0o666)
        os.chmod(CMD_FIFO, 0o666)
    except Exception as e:
        print(f"[net_daemon] Warning mkfifo: {e}", flush=True)

    # Open FIFO non-blocking
    fifo_fd = os.open(CMD_FIFO, os.O_RDONLY | os.O_NONBLOCK)

    # Initial state update
    update_state()

    last_poll = time.time()
    buf = ""

    while True:
        now = time.time()
        # Periodic update every 3 seconds
        if now - last_poll >= 3.0:
            last_poll = now
            update_state()

        # Check FIFO for commands
        r, _, _ = select.select([fifo_fd], [], [], 0.5)
        if r:
            try:
                chunk = os.read(fifo_fd, 1024).decode("utf-8", errors="ignore")
                if chunk:
                    buf += chunk
                    while "\n" in buf:
                        cmd, buf = buf.split("\n", 1)
                        handle_cmd(cmd)
            except Exception as e:
                time.sleep(0.1)

if __name__ == "__main__":
    main()
