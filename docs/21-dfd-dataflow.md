# 21 — Data Flow Diagram (DFD)

> *Versi Bahasa Indonesia: [21-dfd-dataflow-id.md](21-dfd-dataflow-id.md)*

Complete Data Flow Diagram (DFD) documentation for the Pioneer DJ XDJ-RX3 (`rbp`)
standalone player runtime on Raspberry Pi 4B / embedded Linux.

This document describes how data, control signals, audio streams, and user inputs
flow between the physical hardware, the kernel, the userland shims (`LD_PRELOAD`),
helper daemons, and the proprietary ARM32 soft-float `rbp` binary.

---

## 1. DFD Level 0: Context Diagram

The Context Diagram defines the system boundary for **Sistem Standalone Player XDJ-RX3**
and its interactions with external entities (performer, peripherals, storage, audio DAC,
display, network peers, and remote management).

```mermaid
graph TD
    %% External Entities
    DJ["Performer / DJ"]
    CTRL["Hardware DJ Controller<br/>(Pioneer DDJ-FLX4 / FLX6 / MIDI)"]
    POINTER["Pointing & Touch Peripherals<br/>(USB Touchscreen / Mouse / Keyboard)"]
    USB_DRIVE["USB Flash Drives<br/>(FAT32/exFAT Rekordbox Export Media)"]
    AUDIO_HW["Audio Output Interface<br/>(FLX4 4-Ch USB DAC & HDMI Audio Sink)"]
    DISP_HW["Display Panel / Monitor<br/>(HDMI 1280x800 @ 60Hz vc4drmfb)"]
    LAN_PEER["Pro DJ Link LAN Peers<br/>(CDJ-3000, rekordbox PC/Mac, NXS-GW)"]
    WEB_USER["Remote Operator / Browser<br/>(Web Admin HTTP:5904 & VNC Client)"]

    %% Central Process
    SYS["<b>[0.0] Sistem Runtime Standalone XDJ-RX3</b><br/>(rbp Player Engine + Shims + Daemons)"]

    %% Data Flows
    DJ -->|"Physical manipulation (knobs, faders, jog, pads)"| CTRL
    CTRL -->|"MIDI Messages (Note On/Off, CC, Pitch Bend, Jog pulses)"| SYS
    SYS -->|"Status LED feedback & VU meter levels (MIDI / SysEx)"| CTRL

    POINTER -->|"Raw input events (EV_ABS, EV_REL, EV_KEY)"| SYS

    USB_DRIVE -->|"Storage block data (/dev/sdX1, export.pdb, ANLZ, Audio Files)"| SYS
    SYS -->|"Mount / unmount signals & state updates"| USB_DRIVE

    SYS -->|"Audio Stream 4-Channel S24_3LE (Master RCA + Headphone Cue)"| AUDIO_HW
    SYS -->|"Audio Stream Stereo Mirror (HDMI PCM IEC958 subframe)"| AUDIO_HW

    SYS -->|"Framebuffer RGB565 1280x800 @ 60 FPS"| DISP_HW

    LAN_PEER <-->|"UDP 50000 Pro DJ Link Packets (Device Announce, Beat Sync)"| SYS

    WEB_USER <-->|"HTTP Requests / Form POST (Port 5904)"| SYS
    SYS -->|"RFB Video Stream / VNC Frames"| WEB_USER
```

---

## 2. DFD Level 1: System Decomposition

DFD Level 1 decomposes the system into 8 core functional processes and 5 persistent/ephemeral data stores:

*   **1.0 Input & Control Surface Management**: Translates MIDI and evdev inputs into `rbp` ABI keycodes, and generates LED/VU feedback.
*   **2.0 Touch & Pointing Emulation**: Discovers pointer devices, maps coordinates, and simulates the TI TSC2007 resistive touchscreen protocol.
*   **3.0 Core Rekordbox Player Engine**: The extracted XDJ-RX3 ARM32 soft-float player (`rbp`, `PlayEngine`, `DeviceSQL`, `edb_streamd`).
*   **4.0 USB Media Storage & Hotplug Watcher**: Detects USB insertions, validates rekordbox database presence, mounts, and alerts `rbp`.
*   **5.0 Rendering & Display Pipeline**: Interposes framebuffer ioctls, provides 60 Hz vblank pacing, and composites cursor/top-menu overlays.
*   **6.0 Audio Processing & Stream Routing**: Intercepts 3 stereo streams from `rbp`, packs 24-bit samples into 4-channel ALSA frames, and mirrors master to HDMI.
*   **7.0 Network & Pro DJ Link Interception**: Rewrites hardcoded `eth0` interface queries to active interfaces (`wlan0`).
*   **8.0 Monitoring, Web Config & Remote Access**: Provides web management UI (`confscreen.py`), VNC frame streaming (`vncserve`), and health checks.

```mermaid
graph TB
    %% External Entities
    CTRL["Hardware Controller<br/>(DDJ-FLX4 / MIDI)"]
    POINTER["Input Peripherals<br/>(Mouse / Touch)"]
    USB_DEV["USB Storage Media"]
    AUDIO_SINK["Audio Out (FLX4 / HDMI)"]
    DISP["HDMI Monitor"]
    LAN["Pro DJ Link LAN"]
    OPERATOR["Web Browser / VNC Client"]

    %% Data Stores
    D1[("<b>D1: Media Storage</b><br/>export.pdb, exportExt.pdb, ANLZ, MP3/WAV/FLAC")]
    D2[("<b>D2: Configuration Files</b><br/>rb.conf, rb.local.conf")]
    D3[("<b>D3: IPC Pipes & Shared Memory</b><br/>FIFO /tmp/udev_usb*, pipe /dev/tsc2007*, shmstate")]
    D4[("<b>D4: Framebuffer Memory</b><br/>/dev/fb0 (VC4 DRM Framebuffer)")]
    D5[("<b>D5: Logs & Runtime Metrics</b><br/>/tmp/*.log, /run/rblive4/, /proc/")]

    %% Processes
    P1["<b>1.0 Input & Control Surface Management</b><br/>(knobshim.so / midi_io / map_flx4)"]
    P2["<b>2.0 Touch & Pointing Emulation</b><br/>(fbshim.so / pointsrc / tscfake)"]
    P3["<b>3.0 Core Rekordbox Player Engine</b><br/>(rbp / PlayEngine / DeviceSQL / edb_streamd)"]
    P4["<b>4.0 USB Media Hotplug Watcher</b><br/>(usb-watch.sh / mount manager)"]
    P5["<b>5.0 Rendering & Display Pipeline</b><br/>(libdirectfb_fbdev.so / fb_shim.so)"]
    P6["<b>6.0 Audio Processing & Routing</b><br/>(audioshim.so / s24pack / mirror_policy)"]
    P7["<b>7.0 Network & Pro DJ Link Interception</b><br/>(netshim.so / netalias)"]
    P8["<b>8.0 Monitoring & Web Configuration</b><br/>(confscreen.py / vncserve / doctor.sh)"]

    %% Flows - Process 1.0 (Controls)
    CTRL -->|"Raw MIDI In (0x90/0xB0)"| P1
    D2 -.->|"Load keymaps & configs"| P1
    P1 -->|"send_rx_key() / ABI keycode"| P3
    P3 -->|"Engine LED state & VU Level (g_vu_peak)"| P1
    P1 -->|"MIDI Feedback (LED Note & VU CC)"| CTRL
    P1 -.->|"Write event logs"| D5

    %% Flows - Process 2.0 (Touch/Pointing)
    POINTER -->|"Evdev events (/dev/input/event*)"| P2
    P2 -->|"6-byte packet {flag, pad, x, y}"| D3
    D3 -->|"Read /dev/tsc2007_2-0048"| P3
    P2 -->|"Keycode tap (QUANTIZE shortcut)"| P3

    %% Flows - Process 3.0 (Core Engine)
    D1 -->|"Read library metadata & track data"| P3
    P3 -->|"Audio streams (Master, Phone, Booth)"| P6
    P3 -->|"Render UI Surface (1280x800 RGB565)"| P5
    P3 -.->|"Write playback status"| D5

    %% Flows - Process 4.0 (USB Management)
    USB_DEV -->|"Kernel uevent block device (/sys/block/sd*)"| P4
    P4 -->|"Mount partition & bind into chroot"| D1
    P4 -->|"Write 'mount /media/usb1/sda1'"| D3
    D3 -->|"Notify mount event FIFO"| P3

    %% Flows - Process 5.0 (Display)
    P5 -->|"FBIOPAN_DISPLAY / Write Pixels"| D4
    D4 -->|"Signal display scanout"| DISP
    P5 -.->|"60 FPS pacing control"| D5

    %% Flows - Process 6.0 (Audio)
    P1 -.->|"Update g_master_gain in shmstate"| D3
    D3 -.->|"Read g_master_gain"| P6
    P6 -->|"ALSA snd_pcm_writei (4-ch S24_3LE)"| AUDIO_SINK
    P6 -->|"ALSA HDMI Out (IEC958 subframe)"| AUDIO_SINK
    P6 -.->|"Update g_vu_peak"| D3
    P6 -.->|"Audio diagnostic log"| D5

    %% Flows - Process 7.0 (Network)
    LAN <-->|"Raw Ethernet/WLAN Packets"| P7
    P7 <-->|"Rewritten ioctl (eth0 -> wlan0)"| P3

    %% Flows - Process 8.0 (Web & Config)
    D5 -.->|"Read system metrics, service status & logs"| P8
    OPERATOR <-->|"Configuration & GUI control"| P8
    P8 -->|"Update setting values"| D2
    D4 -.->|"Grab frame /dev/fb0"| P8
    P8 -->|"Stream RFB / VNC"| OPERATOR
```

---

## 3. DFD Level 2: Detailed Process Workflows

### 3.1 Process 1.0: Physical Controls & LED/VU Feedback Loop

Detailed data flow inside [ctrlshim.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/ctrlshim.c), [map_flx4.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/map_flx4.c), and [rbp_bridge.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/rbp_bridge.c).

```mermaid
graph TD
    MIDI_IN["ALSA Sequencer Port (/dev/snd/seq)"]
    EVDEV_IN["Keyboard / Evdev (/dev/input/event*)"]

    subgraph P1["Process 1.0: Input & Control Surface Management"]
        P1_1["1.1 Polling & Receiver Thread<br/>(midi_io.c / evdev_io.c)"]
        P1_2["1.2 Control Surface Mapper<br/>(map_flx4.c / map_kbd.c)"]
        P1_3["1.3 ABI Keycode Injection<br/>(rbp_bridge.c: send_rx_key)"]
        P1_4["1.4 State Filter & Rescale VU<br/>(rbp_vu.c / led_table.c)"]
        P1_5["1.5 Outgoing MIDI Generator<br/>(midi_io.c: send_note / send_cc)"]
    end

    RBP_KM["rbp KeyManager (0x004e...)"]
    RBP_ENG["rbp LedManager & Peak Meter Hook"]
    LED_OUT["Controller Panel LEDs & Level Meters"]

    MIDI_IN -->|"Raw Packet (3 bytes)"| P1_1
    EVDEV_IN -->|"input_event struct"| P1_1
    P1_1 -->|"Normalized event"| P1_2
    P1_2 -->|"Lookup RX3 Keycode (0x4101=PLAY, 0x4311=LOAD)"| P1_3
    P1_3 -->|"Call rbp singleton function"| RBP_KM

    RBP_ENG -->|"Read LED bitmask & audio peak"| P1_4
    P1_4 -->|"LED note intensity & VU level ramp"| P1_5
    P1_5 -->|"MIDI Transmit"| LED_OUT
```

---

### 3.2 Process 4.0: USB Hotplug & Rekordbox Database Ingestion

Detailed flow of [usb-watch.sh](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/device/usb-watch.sh) handling media insertion and library attachment.

```mermaid
graph TD
    USB_KERNEL["Kernel Uevent (/sys/block/sd*)"]

    subgraph P4["Process 4.0: USB Media Storage Watcher"]
        P4_1["4.1 Block Device Monitor Loop<br/>(usb-watch.sh)"]
        P4_2["4.2 Database Probe<br/>(probe_export: check PIONEER/rekordbox/export.pdb)"]
        P4_3["4.3 Filesystem Mount & Chroot Bind<br/>(mount -t vfat/exfat to /media/usbN/sda1)"]
        P4_4["4.4 FIFO Notification<br/>(echo 'mount ...' > /tmp/udev_usb1)"]
        P4_5["4.5 Watchdog /proc fd Verifier<br/>(Confirm rbp opens export.pdb)"]
    end

    FIFO_IPC["FIFO /tmp/udev_usb1"]
    RBP_MOUNT["rbp UsbMountManager"]
    DBSQL["DeviceSQL Engine (edb_streamd)"]

    USB_KERNEL -->|"Flash drive inserted"| P4_1
    P4_1 -->|"Block node /dev/sdX1"| P4_2
    P4_2 -->|"Verified valid export.pdb"| P4_3
    P4_3 -->|"Mount completed"| P4_4
    P4_4 -->|"Send mount command"| FIFO_IPC
    FIFO_IPC -->|"Receive mount line"| RBP_MOUNT
    RBP_MOUNT -->|"Initialize library indexing"| DBSQL
    DBSQL -->|"Open fd for export.pdb"| P4_5
    P4_5 -->|"Mark slot as active & confirmed"| P4_1
```

---

### 3.3 Process 6.0: Multi-Channel Audio Processing & HDMI Mirroring

Detailed flow inside [audioshim.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/audioshim.c), [s24pack.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/s24pack.c), and [mirror_policy.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/mirror_policy.c).

```mermaid
graph TD
    subgraph P3["rbp Core Player Engine"]
        S0["Stream 0: Master (Stereo S24_LE 44.1kHz)"]
        S1["Stream 1: Headphone / Cue (Stereo S24_LE 44.1kHz)"]
        S2["Stream 2: Booth (Stereo S24_LE 44.1kHz)"]
    end

    subgraph P6["Process 6.0: Audio Processing (audioshim.so)"]
        P6_1["6.1 Buffer Staging Interceptor<br/>(Hold Stream 1 & 2 until Stream 0 arrives)"]
        P6_2["6.2 Master Gain Scaling<br/>(Apply gain from g_master_gain)"]
        P6_3["6.3 24-bit Format Packer<br/>(s24pack.c: S24_LE 32-bit container -> S24_3LE 3-byte)"]
        P6_4["6.4 Clock Pacing & Synchronous Flush<br/>(snd_pcm_writei 4-Channel Interleaved)"]
        P6_5["6.5 HDMI Audio Mirror Policy<br/>(mirror_policy.c: Non-blocking ring buffer + IEC958 subframe)"]
    end

    ALSA_FLX4["ALSA Device: hw:CARD=DDJFLX4,DEV=0<br/>(Ch 1-2: Master RCA, Ch 3-4: Headphone Cue)"]
    ALSA_HDMI["ALSA Device: hw:CARD=bcm2835_hdmi<br/>(Direct HDMI PCM Audio to TV / Soundbar)"]

    S1 -->|"Stage cue buffer"| P6_1
    S2 -->|"Stage booth buffer"| P6_1
    S0 -->|"Trigger sync flush"| P6_1

    P6_1 -->|"4-Ch Raw Audio"| P6_2
    P6_2 -->|"Scaled Samples"| P6_3
    P6_3 -->|"Interleaved S24_3LE Block"| P6_4
    P6_4 -->|"Zero-underrun Direct PCM write"| ALSA_FLX4

    P6_2 -->|"Master Pair Copy"| P6_5
    P6_5 -->|"Async Ring Buffer (PPM Drift Protected)"| ALSA_HDMI
```

---

### 3.4 Process 2.0 & 5.0: Touch Input Emulation & Display Framebuffer Pipeline

Detailed flow between [pointsrc.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/pointsrc.c), [tscfake.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/tscfake.c), [fb_shim.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/fb_shim.c), and DirectFB.

```mermaid
graph TD
    POINTER_HW["Hardware Mouse / Touch Panel"]
    RBP_RENDER["rbp GUI Thread (DirectFB Render)"]

    subgraph P2["Process 2.0: Touch & Pointing Emulation"]
        P2_1["2.1 Device Auto-Discovery<br/>(pointsrc.c: scan evdev capability BTN_TOUCH / REL_X)"]
        P2_2["2.2 Coordinate Normalization & Reflection<br/>(point_xform.c: map raw -> 1280x800 & undo 1279-x)"]
        P2_3["2.3 Fake tsc2007 Serializer<br/>(tscfake.c: write 6-byte burst to pipe /dev/tsc2007*)"]
    end

    subgraph P5["Process 5.0: Display & Compositor"]
        P5_1["5.1 FBIOPAN_DISPLAY Interception & Pacer<br/>(fb_shim.c: Clock 60Hz sleep to prevent spinning CPU core)"]
        P5_2["5.2 Software Mouse Cursor Compositor<br/>(cursor_paint.c & fb_cursor.c)"]
        P5_3["5.3 Swipe-Down Quick Menu OSD<br/>(menu_draw.c & menu_zone.c)"]
        P5_4["5.4 Layer Blit & Scanout Driver<br/>(libdirectfb_fbdev.so)"]
    end

    RBP_UI["rbp Touch Event Handler"]
    FB0["Physical Framebuffer /dev/fb0 (vc4drmfb)"]

    POINTER_HW -->|"Evdev raw packets"| P2_1
    P2_1 -->|"Coordinates (rx, ry)"| P2_2
    P2_2 -->|"tsc2007 Packet"| P2_3
    P2_3 -->|"Read pipe"| RBP_UI

    P2_2 -->|"Mouse cursor coordinates"| P5_2

    RBP_RENDER -->|"Render layer RGB565"| P5_4
    P5_4 -->|"Trigger pan/present"| P5_1
    P5_1 -->|"Single page buffer"| P5_2
    P5_2 -->|"Draw arrow glyph (if mouse active)"| P5_3
    P5_3 -->|"Overlay menu panel (if swipe down detected)"| FB0
```

---

## 4. Data Dictionary (Kamus Data)

| Data Element | Format / Specification | Source Process | Target Process | Description |
|---|---|---|---|---|
| **MIDI In Message** | 3-byte ALSA MIDI packet `[status, data1, data2]` | Hardware Controller | P1.1 `midi_io.c` | Physical controller events: buttons (Note On/Off), faders/knobs (CC), jog wheel relative movement. |
| **RX3 Keycode** | `uint32_t` (e.g., `0x4101` Play, `0x4311` Load) | P1.2 `map_flx4.c` | P1.3 `rbp_bridge.c` | Pioneer internal UI/transport keycode injected directly into `rbp`'s `KeyManager::send_rx_key()`. |
| **tsc2007 Packet** | 6-byte binary struct: `{u8 flag; u8 pad; u16 x; u16 y}` | P2.3 `tscfake.c` | P3.0 `rbp` UI Thread | Emulated packet of TI TSC2007 resistive touch controller fed into `/dev/tsc2007_2-0048`. |
| **USB Mount Line** | ASCII string: `"mount /media/usb1/sda1\n"` | P4.4 `usb-watch.sh` | P3.0 `rbp` `UsbMountManager` | Written to FIFO `/tmp/udev_usb1` to trigger device library mount in `rbp`. |
| **rekordbox DB** | DeviceSQL / SQLite binary format (`export.pdb`, `exportExt.pdb`) | D1 USB Media | P3.0 `edb_streamd` / `rbp` | Tracks, playlists, cue points, waveform peaks (`ANLZ`), and album art references. |
| **rbp Audio Period** | 32-bit container PCM Interleaved S24_LE (44.1 kHz, 64 frames) | P3.0 `rbp` Audio Engine | P6.1 `audioshim.c` | 3 distinct stereo streams: Stream 0 (Master), Stream 1 (Headphones/Cue), Stream 2 (Booth). |
| **FLX4 Audio Stream** | 24-bit packed PCM Interleaved S24_3LE (44.1 kHz, 4 channels) | P6.3 `s24pack.c` | ALSA USB Audio Driver | Physical 4-channel output: Ch 1-2 Master Out (RCA), Ch 3-4 Headphone Monitor (Jack 3.5mm). |
| **HDMI Mirror Stream** | IEC 60958 Subframe Audio (`IEC958_SUBFRAME_LE`) | P6.5 `mirror_policy.c` | ALSA VC4 HDMI Driver | Non-blocking master audio copy directed to HDMI sink, protected against clock drift (~155 ppm). |
| **Shared State (`shmstate`)** | Global C variables: `g_master_gain`, `g_vu_peak` | P1.0 `knobshim` / P6.0 `audioshim` | Inter-shim IPC | Bidirectional shared memory exchange: master volume knob gain and audio VU peak levels. |
| **Pro DJ Link Packet** | UDP Datagram Port 50000 (Magic: `Qspt1WmJOL`) | P7.0 `netshim.so` | External LAN Peers | Beatgrid synchronization, player status broadcast, and remote player link communication. |

---

## 5. Preload Ordering & Execution Hierarchy

The runtime enforces a strict `LD_PRELOAD` ordering in [scripts/shims/README.md](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/README.md):

```
LD_PRELOAD=/usr/lib/crashcatch.so:/usr/lib/netshim.so:/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so
```

1. **`crashcatch.so`** (First): Traps fatal signals (SIGSEGV/SIGBUS) and writes PC/LR registers to `/tmp/crash.log`.
2. **`netshim.so`** (Second, before `fbshim.so`): Both shims interpose `ioctl()`. `netshim` must intercept `eth0` queries first and delegate everything else via `dlsym(RTLD_NEXT)` to `fbshim.so`.
3. **`fbshim.so`** (Third): Interposes framebuffer ioctls, paces 60 FPS presentations, and creates fake `/dev/tsc2007_2-0048`.
4. **`knobshim.so`** (Fourth, before `audioshim.so`): Defines data symbols (`g_master_gain`) read by `audioshim.so`.
5. **`audioshim.so`** (Fifth): Defines data symbols (`g_vu_peak`) read back by `knobshim.so`.
