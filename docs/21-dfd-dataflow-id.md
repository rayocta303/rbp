# 21 — Diagram Alir Data / Data Flow Diagram (DFD)

> *Versi Bahasa Inggris: [21-dfd-dataflow.md](21-dfd-dataflow.md)*

Dokumentasi lengkap Diagram Alir Data (Data Flow Diagram / DFD) untuk sistem runtime
standalone player Pioneer DJ XDJ-RX3 (`rbp`) pada platform Raspberry Pi 4B / Linux tertanam (embedded Linux).

Dokumen ini menjelaskan bagaimana data, sinyal kontrol, aliran audio (*audio streams*), dan input pengguna
mengalir di antara perangkat keras fisik, kernel Linux, pustaka *shim* ruang pengguna (*userspace* `LD_PRELOAD`),
daemon pembantu (*helper daemons*), serta biner asli ARM32 *soft-float* `rbp`.

---

## 1. DFD Level 0: Diagram Konteks (Context Diagram)

Diagram Konteks mendefinisikan batasan sistem untuk **Sistem Runtime Standalone XDJ-RX3**
serta interaksinya dengan entitas-entitas eksternal (DJ/penampil, perangkat kontrol fisik, media penyimpanan,
antarmuka audio DAC, monitor tampilan, rekan jaringan Pro DJ Link, dan manajemen jarak jauh).

```mermaid
graph TD
    %% Entitas Eksternal
    DJ["DJ / Penampil"]
    CTRL["Hardware DJ Controller<br/>(Pioneer DDJ-FLX4 / FLX6 / MIDI)"]
    POINTER["Perangkat Penunjuk & Sentuh<br/>(USB Touchscreen / Mouse / Keyboard)"]
    USB_DRIVE["Media Penyimpanan USB<br/>(Flashdisk FAT32/exFAT Export Rekordbox)"]
    AUDIO_HW["Antarmuka Output Audio<br/>(FLX4 4-Ch USB DAC & HDMI Audio Sink)"]
    DISP_HW["Panel Layar / Monitor<br/>(HDMI 1280x800 @ 60Hz vc4drmfb)"]
    LAN_PEER["Perangkat Jaringan Pro DJ Link<br/>(CDJ-3000, rekordbox PC/Mac, NXS-GW)"]
    WEB_USER["Operator Jarak Jauh / Browser<br/>(Web Admin HTTP:5904 & VNC Client)"]

    %% Proses Utama
    SYS["<b>[0.0] Sistem Runtime Standalone XDJ-RX3</b><br/>(Player Engine rbp + Shims + Daemons)"]

    %% Aliran Data
    DJ -->|"Manipulasi fisik (knob, fader, jog wheel, pad)"| CTRL
    CTRL -->|"Pesan MIDI (Note On/Off, CC, Pitch Bend, Jog pulses)"| SYS
    SYS -->|"Umpan balik status LED & level VU meter (MIDI / SysEx)"| CTRL

    POINTER -->|"Event raw input evdev (EV_ABS, EV_REL, EV_KEY)"| SYS

    USB_DRIVE -->|"Data blok storage (/dev/sdX1, export.pdb, ANLZ, File Audio)"| SYS
    SYS -->|"Sinyal mount / unmount & pembaruan status media"| USB_DRIVE

    SYS -->|"Audio Stream 4-Channel S24_3LE (Master RCA + Headphone Cue)"| AUDIO_HW
    SYS -->|"Audio Stream Stereo Mirror (HDMI PCM IEC958 subframe)"| AUDIO_HW

    SYS -->|"Buffer Tampilan RGB565 1280x800 @ 60 FPS"| DISP_HW

    LAN_PEER <-->|"Paket UDP 50000 Pro DJ Link (Device Announce, Beat Sync)"| SYS

    WEB_USER <-->|"Permintaan HTTP / Form POST (Port 5904)"| SYS
    SYS -->|"Stream Video RFB / Frame Gambar VNC"| WEB_USER
```

---

## 2. DFD Level 1: Dekomposisi Sistem

DFD Level 1 memecah sistem utama menjadi 8 sub-proses fungsional dan 5 penyimpan data (*data stores*):

*   **1.0 Manajemen Kontrol & Input Fisik**: Menerjemahkan input MIDI dan evdev menjadi kode tombol (*keycode*) ABI internal `rbp`, serta menghasilkan sinyal umpan balik LED/VU.
*   **2.0 Emulasi Layar Sentuh & Pointing**: Menemukan perangkat penunjuk (*auto-discovery*), menormalisasi koordinat, dan mensimulasikan protokol kontroler resistif TI TSC2007.
*   **3.0 Core Rekordbox Player Engine**: Biner asli pemutar XDJ-RX3 ARM32 *soft-float* (`rbp`, `PlayEngine`, `DeviceSQL`, `edb_streamd`).
*   **4.0 Watcher Media USB & Manajemen Hotplug**: Mendeteksi penyematan USB, memvalidasi keberadaan database rekordbox, melakukan proses *mount*, dan memberi sinyal ke `rbp`.
*   **5.0 Pipeline Rendering & Tampilan**: Mengintersepsi ioctl framebuffer, menyediakan sinkronisasi *pacing* 60 Hz (*vblank*), dan menggabungkan overlay kursor serta menu cepat.
*   **6.0 Pemrosesan Audio & Pengalihan Stream**: Menangkap 3 stream stereo dari `rbp`, mengemas sampel 24-bit ke dalam frame ALSA 4-kanal, dan menduplikasi master ke HDMI.
*   **7.0 Intersepsi Jaringan & Pro DJ Link**: Menulis ulang query nama antarmuka `eth0` yang terkode mati (*hardcoded*) ke antarmuka yang aktif (`wlan0`).
*   **8.0 Monitoring, Konfigurasi Web & Akses Jarak Jauh**: Menyediakan antarmuka manajemen web (`confscreen.py`), streaming layar VNC (`vncserve`), dan pemantau kesehatan sistem.

```mermaid
graph TB
    %% Entitas Eksternal
    CTRL["Hardware Controller<br/>(DDJ-FLX4 / MIDI)"]
    POINTER["Perangkat Penunjuk<br/>(Mouse / Touch)"]
    USB_DEV["Penyimpanan USB"]
    AUDIO_SINK["Output Audio (FLX4 / HDMI)"]
    DISP["Monitor HDMI"]
    LAN["LAN Pro DJ Link"]
    OPERATOR["Web Browser / Client VNC"]

    %% Penyimpan Data (Data Stores)
    D1[("<b>D1: Media Penyimpanan</b><br/>export.pdb, exportExt.pdb, ANLZ, MP3/WAV/FLAC")]
    D2[("<b>D2: File Konfigurasi</b><br/>rb.conf, rb.local.conf")]
    D3[("<b>D3: IPC Pipes & Shared Memory</b><br/>FIFO /tmp/udev_usb*, pipe /dev/tsc2007*, shmstate")]
    D4[("<b>D4: Memori Framebuffer</b><br/>/dev/fb0 (VC4 DRM Framebuffer)")]
    D5[("<b>D5: Log & Metrik Runtime</b><br/>/tmp/*.log, /run/rblive4/, /proc/")]

    %% Proses-Proses
    P1["<b>1.0 Manajemen Kontrol & Input Fisik</b><br/>(knobshim.so / midi_io / map_flx4)"]
    P2["<b>2.0 Emulasi Layar Sentuh & Pointing</b><br/>(fbshim.so / pointsrc / tscfake)"]
    P3["<b>3.0 Core Rekordbox Player Engine</b><br/>(rbp / PlayEngine / DeviceSQL / edb_streamd)"]
    P4["<b>4.0 Watcher Media USB & Hotplug</b><br/>(usb-watch.sh / mount manager)"]
    P5["<b>5.0 Pipeline Rendering & Tampilan</b><br/>(libdirectfb_fbdev.so / fb_shim.so)"]
    P6["<b>6.0 Pemrosesan Audio & Pengalihan Stream</b><br/>(audioshim.so / s24pack / mirror_policy)"]
    P7["<b>7.0 Intersepsi Jaringan Pro DJ Link</b><br/>(netshim.so / netalias)"]
    P8["<b>8.0 Monitoring & Konfigurasi Web</b><br/>(confscreen.py / vncserve / doctor.sh)"]

    %% Aliran Data - Proses 1.0 (Kontrol)
    CTRL -->|"Raw MIDI In (0x90/0xB0)"| P1
    D2 -.->|"Muat pemetaan tombol & konfigurasi"| P1
    P1 -->|"send_rx_key() / ABI keycode"| P3
    P3 -->|"Status LED & level puncak audio (g_vu_peak)"| P1
    P1 -->|"Umpan balik MIDI (Note LED & CC VU)"| CTRL
    P1 -.->|"Tulis catatan log event"| D5

    %% Aliran Data - Proses 2.0 (Sentuh/Pointing)
    POINTER -->|"Event evdev (/dev/input/event*)"| P2
    P2 -->|"Paket 6-byte {flag, pad, x, y}"| D3
    D3 -->|"Baca /dev/tsc2007_2-0048"| P3
    P2 -->|"Keycode tap (Pintasan sentuh QUANTIZE)"| P3

    %% Aliran Data - Proses 3.0 (Core Engine)
    D1 -->|"Baca metadata pustaka lagu & file audio"| P3
    P3 -->|"Audio stream (Master, Phone/Cue, Booth)"| P6
    P3 -->|"Render permukaan GUI (1280x800 RGB565)"| P5
    P3 -.->|"Tulis status putar lagu"| D5

    %% Aliran Data - Proses 4.0 (Manajemen USB)
    USB_DEV -->|"Kernel uevent block device (/sys/block/sd*)"| P4
    P4 -->|"Mount partisi & bind ke dalam chroot"| D1
    P4 -->|"Tulis string 'mount /media/usb1/sda1'"| D3
    D3 -->|"Notifikasi event mount FIFO"| P3

    %% Aliran Data - Proses 5.0 (Tampilan)
    P5 -->|"FBIOPAN_DISPLAY / Tulis Pixel"| D4
    D4 -->|"Sinyal pemindaian layar (scanout)"| DISP
    P5 -.->|"Kontrol sinkronisasi 60 FPS"| D5

    %% Aliran Data - Proses 6.0 (Audio)
    P1 -.->|"Perbarui g_master_gain di shmstate"| D3
    D3 -.->|"Baca nilai g_master_gain"| P6
    P6 -->|"ALSA snd_pcm_writei (4-ch S24_3LE)"| AUDIO_SINK
    P6 -->|"ALSA Output HDMI (IEC958 subframe)"| AUDIO_SINK
    P6 -.->|"Perbarui g_vu_peak"| D3
    P6 -.->|"Log diagnostik audio"| D5

    %% Aliran Data - Proses 7.0 (Jaringan)
    LAN <-->|"Paket Raw Ethernet/WLAN"| P7
    P7 <-->|"Ioctl yang dialihkan (eth0 -> wlan0)"| P3

    %% Aliran Data - Proses 8.0 (Web & Konfigurasi)
    D5 -.->|"Baca metrik sistem, status service & log"| P8
    OPERATOR <-->|"Konfigurasi & kontrol GUI"| P8
    P8 -->|"Perbarui parameter pengaturan"| D2
    D4 -.->|"Ambil frame /dev/fb0"| P8
    P8 -->|"Stream RFB / VNC"| OPERATOR
```

---

## 3. DFD Level 2: Alur Kerja Rinci Sub-Sistem Kritis

### 3.1 Proses 1.0: Siklus Kontrol Fisik & Umpan Balik LED/VU Meter

Alur data internal di dalam [ctrlshim.c](ctrlshim.c), [map_flx4.c](map_flx4.c), dan [rbp_bridge.c](rbp_bridge.c).

```mermaid
graph TD
    MIDI_IN["Port Sequencer ALSA MIDI (/dev/snd/seq)"]
    EVDEV_IN["Keyboard / Perangkat Evdev (/dev/input/event*)"]

    subgraph P1["Proses 1.0: Manajemen Kontrol & Input Fisik"]
        P1_1["1.1 Thread Polling & Penerima Event<br/>(midi_io.c / evdev_io.c)"]
        P1_2["1.2 Mapper Permukaan Kontroler<br/>(map_flx4.c / map_kbd.c)"]
        P1_3["1.3 Injeksi Keycode ABI Internal<br/>(rbp_bridge.c: send_rx_key)"]
        P1_4["1.4 Filter Status & Penskalaan VU<br/>(rbp_vu.c / led_table.c)"]
        P1_5["1.5 Generator Paket MIDI Keluar<br/>(midi_io.c: send_note / send_cc)"]
    end

    RBP_KM["rbp KeyManager (0x004e...)"]
    RBP_ENG["rbp LedManager & Hook Level Meter"]
    LED_OUT["LED Panel & Indikator Meter Fisik"]

    MIDI_IN -->|"Paket Raw MIDI (3 bytes)"| P1_1
    EVDEV_IN -->|"Struct input_event"| P1_1
    P1_1 -->|"Event ternormalisasi"| P1_2
    P1_2 -->|"Lookup Keycode RX3 (0x4101=PLAY, 0x4311=LOAD)"| P1_3
    P1_3 -->|"Panggil fungsi singleton rbp"| RBP_KM

    RBP_ENG -->|"Baca bitmask status LED & level puncak audio"| P1_4
    P1_4 -->|"Intensitas note LED & nilai CC VU meter"| P1_5
    P1_5 -->|"Transmisi MIDI Out"| LED_OUT
```

---

### 3.2 Proses 4.0: Alur Hotplug USB & Pemasukan Database Rekordbox

Alur penanganan penyematan flashdisk hingga track siap diakses oleh mesin player di [usb-watch.sh](../scripts/device/usb-watch.sh).

```mermaid
graph TD
    USB_KERNEL["Kernel Uevent Block Device (/sys/block/sd*)"]

    subgraph P4["Proses 4.0: Watcher Media USB"]
        P4_1["4.1 Loop Pemantau Blok Perangkat<br/>(usb-watch.sh)"]
        P4_2["4.2 Pemeriksaan Validitas Database<br/>(probe_export: verifikasi PIONEER/rekordbox/export.pdb)"]
        P4_3["4.3 Mount Berkas Sistem & Bind Chroot<br/>(mount -t vfat/exfat ke /media/usbN/sda1)"]
        P4_4["4.4 Notifikasi Event Melalui FIFO<br/>(echo 'mount ...' > /tmp/udev_usb1)"]
        P4_5["4.5 Pengawas Verifikasi /proc fd<br/>(Memastikan rbp membuka file export.pdb)"]
    end

    FIFO_IPC["FIFO IPC /tmp/udev_usb1"]
    RBP_MOUNT["rbp UsbMountManager"]
    DBSQL["Engine DeviceSQL (edb_streamd)"]

    USB_KERNEL -->|"Flashdisk dicolokkan"| P4_1
    P4_1 -->|"Node blok perangkat /dev/sdX1"| P4_2
    P4_2 -->|"Terverifikasi ada file database valid"| P4_3
    P4_3 -->|"Mount berhasil dilakukan"| P4_4
    P4_4 -->|"Kirim pesan baris mount"| FIFO_IPC
    FIFO_IPC -->|"Terima baris perintah mount"| RBP_MOUNT
    RBP_MOUNT -->|"Inisialisasi pembacaan katalog library"| DBSQL
    DBSQL -->|"Buka file descriptor export.pdb"| P4_5
    P4_5 -->|"Tandai slot USB aktif & siap digunakan"| P4_1
```

---

### 3.3 Proses 6.0: Pemrosesan Audio Multi-Kanal & Duplikasi HDMI

Alur pengemasan audio di [audioshim.c](audioshim.c), [s24pack.c](s24pack.c), dan [mirror_policy.c](mirror_policy.c).

```mermaid
graph TD
    subgraph P3["Core Player Engine rbp"]
        S0["Stream 0: Master (Stereo S24_LE 44.1kHz)"]
        S1["Stream 1: Headphone / Cue (Stereo S24_LE 44.1kHz)"]
        S2["Stream 2: Booth (Stereo S24_LE 44.1kHz)"]
    end

    subgraph P6["Proses 6.0: Pemrosesan Audio (audioshim.so)"]
        P6_1["6.1 Buffer Staging Interceptor<br/>(Menahan Stream 1 & 2 hingga Stream 0 siap)"]
        P6_2["6.2 Penskalaan Master Gain<br/>(Terapkan penguatan dari g_master_gain)"]
        P6_3["6.3 Pengemas Format 24-bit<br/>(s24pack.c: S24_LE kontainer 32-bit -> S24_3LE padat 3-byte)"]
        P6_4["6.4 Pengatur Clock & Flush Sinkron<br/>(snd_pcm_writei 4-Kanal Interleaved)"]
        P6_5["6.5 Kebijakan Duplikasi Audio HDMI<br/>(mirror_policy.c: Ring buffer non-blocking + subframe IEC958)"]
    end

    ALSA_FLX4["Perangkat ALSA: hw:CARD=DDJFLX4,DEV=0<br/>(Kanal 1-2: Master RCA, Kanal 3-4: Headphone Cue)"]
    ALSA_HDMI["Perangkat ALSA: hw:CARD=bcm2835_hdmi<br/>(Audio PCM Langsung ke Monitor TV / Soundbar)"]

    S1 -->|"Stage buffer headphone"| P6_1
    S2 -->|"Stage buffer booth"| P6_1
    S0 -->|"Picu pengiriman serentak"| P6_1

    P6_1 -->|"Data audio 4-kanal mentah"| P6_2
    P6_2 -->|"Sampel yang telah diskalakan"| P6_3
    P6_3 -->|"Blok data S24_3LE interleaved"| P6_4
    P6_4 -->|"Penulisan direct PCM tanpa underrun"| ALSA_FLX4

    P6_2 -->|"Salinan kanal master"| P6_5
    P6_5 -->|"Async ring buffer (Terlindungi dari drift clock)"| ALSA_HDMI
```

---

### 3.4 Proses 2.0 & 5.0: Emulasi Sentuh & Pipeline Framebuffer Tampilan

Alur manipulasi koordinat input dan tampilan grafis di [pointsrc.c](pointsrc.c), [tscfake.c](tscfake.c), [fb_shim.c](fb_shim.c), dan DirectFB.

```mermaid
graph TD
    POINTER_HW["Hardware Mouse / Panel Sentuh"]
    RBP_RENDER["Thread GUI rbp (Render DirectFB)"]

    subgraph P2["Proses 2.0: Emulasi Layar Sentuh & Pointing"]
        P2_1["2.1 Deteksi Otomatis Perangkat<br/>(pointsrc.c: pindai kapabilitas evdev BTN_TOUCH / REL_X)"]
        P2_2["2.2 Normalisasi Koordinat & Refleksi<br/>(point_xform.c: petakan mentah -> 1280x800 & balikkan 1279-x)"]
        P2_3["2.3 Serializer Palsu tsc2007<br/>(tscfake.c: tulis paket burst 6-byte ke pipe /dev/tsc2007*)"]
    end

    subgraph P5["Proses 5.0: Display & Compositor"]
        P5_1["5.1 Intersepsi FBIOPAN_DISPLAY & Pacer<br/>(fb_shim.c: Tidur sinkronisasi 60Hz agar CPU tidak overload)"]
        P5_2["5.2 Compositor Kursor Mouse Perangkat Lunak<br/>(cursor_paint.c & fb_cursor.c)"]
        P5_3["5.3 Menu Cepat Geser Turun OSD<br/>(menu_draw.c & menu_zone.c)"]
        P5_4["5.4 Driver Blit Layer & Scanout<br/>(libdirectfb_fbdev.so)"]
    end

    RBP_UI["Handler Event Sentuh rbp"]
    FB0["Framebuffer Fisik /dev/fb0 (vc4drmfb)"]

    POINTER_HW -->|"Paket raw evdev"| P2_1
    P2_1 -->|"Koordinat mentah (rx, ry)"| P2_2
    P2_2 -->|"Paket emulasi tsc2007"| P2_3
    P2_3 -->|"Baca dari pipe"| RBP_UI

    P2_2 -->|"Koordinat kursor mouse"| P5_2

    RBP_RENDER -->|"Render layer RGB565"| P5_4
    P5_4 -->|"Picu eksekusi pan/present"| P5_1
    P5_1 -->|"Buffer halaman tunggal"| P5_2
    P5_2 -->|"Gambar kursor panah (jika mouse aktif)"| P5_3
    P5_3 -->|"Gambar overlay menu panel (jika swipe terdeteksi)"| FB0
```

---

## 4. Kamus Data (Data Dictionary)

| Elemen Data | Format / Spesifikasi | Proses Asal | Proses Tujuan | Deskripsi Fungsional |
|---|---|---|---|---|
| **Pesan MIDI Masuk** | Paket ALSA MIDI 3-byte: `[status, data1, data2]` | Controller Hardware | P1.1 `midi_io.c` | Event fisik controller: tombol (Note On/Off), fader/knob (CC), gerakan rotari jog wheel. |
| **Keycode RX3** | Nilai `uint32_t` (contoh: `0x4101` Play, `0x4311` Load Ch 1) | P1.2 `map_flx4.c` | P1.3 `rbp_bridge.c` | Kode tombol firmware asli RX3 yang diinjeksikan langsung ke `KeyManager::send_rx_key()`. |
| **Paket tsc2007** | Struct biner 6-byte: `{u8 flag; u8 pad; u16 x; u16 y}` | P2.3 `tscfake.c` | P3.0 Thread UI `rbp` | Paket emulasi chip kontroler resistif TI TSC2007 yang dialirkan melalui pipa `/dev/tsc2007_2-0048`. |
| **Baris Mount USB** | String ASCII: `"mount /media/usb1/sda1\n"` | P4.4 `usb-watch.sh` | P3.0 `UsbMountManager` | Dituliskan ke FIFO `/tmp/udev_usb1` untuk memicu proses pembacaan partisi media oleh `rbp`. |
| **Database Rekordbox** | File biner DeviceSQL / SQLite (`export.pdb`, `exportExt.pdb`) | D1 Media USB | P3.0 `edb_streamd` / `rbp` | Berisi daftar lagu, playlist, cue points, analisis puncak waveform (`ANLZ`), dan artwork album. |
| **Period Audio rbp** | PCM Interleaved S24_LE kontainer 32-bit (44.1 kHz, 64 frame) | P3.0 Audio Engine `rbp` | P6.1 `audioshim.c` | 3 aliran audio stereo simultan: Stream 0 (Master), Stream 1 (Headphones/Cue), Stream 2 (Booth). |
| **Audio Stream FLX4** | PCM Interleaved S24_3LE padat 24-bit (44.1 kHz, 4 kanal) | P6.3 `s24pack.c` | Driver USB ALSA | Output 4 kanal audio fisik: Kanal 1-2 Master Out (RCA), Kanal 3-4 Headphone Monitor (Jack 3.5mm). |
| **Audio Mirror HDMI** | Subframe Audio IEC 60958 (`IEC958_SUBFRAME_LE`) | P6.5 `mirror_policy.c` | Driver HDMI ALSA VC4 | Duplikasi audio master non-blocking ke monitor HDMI, terhindar dari desinkronisasi clock (~155 ppm). |
| **State Bersama (`shmstate`)** | Variabel global C: `g_master_gain`, `g_vu_peak` | P1.0 `knobshim` / P6.0 `audioshim` | IPC Antar-Shim | Pertukaran memori bersama antar library: penguatan volume knob master dan nilai puncak level VU meter. |
| **Paket Pro DJ Link** | Datagram UDP Port 50000 (Magic: `Qspt1WmJOL`) | P7.0 `netshim.so` | Perangkat LAN Eksternal | Sinkronisasi ketukan beatgrid, pengumuman status pemain, dan komunikasi pertukaran link antar player. |

---

## 5. Urutan Pemuatan `LD_PRELOAD` & Hirarki Eksekusi

Sistem runtime mewajibkan urutan pemuatan pustaka `LD_PRELOAD` yang ketat sebagaimana diatur pada [scripts/shims/README.md](../scripts/shims/README.md):

```
LD_PRELOAD=/usr/lib/crashcatch.so:/usr/lib/netshim.so:/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so
```

1. **`crashcatch.so`** (Urutan Pertama): Menangkap sinyal fatal (SIGSEGV/SIGBUS) dan mencatat register PC/LR ke `/tmp/crash.log`.
2. **`netshim.so`** (Urutan Kedua, sebelum `fbshim.so`): Keduanya mengintersepsi fungsi `ioctl()`. `netshim` harus menangkap query `eth0` terlebih dahulu dan mendelegasikan panggilan lainnya via `dlsym(RTLD_NEXT)` ke `fbshim.so`.
3. **`fbshim.so`** (Urutan Ketiga): Mengintersepsi ioctl framebuffer, mengatur pacing frame 60 FPS, serta membuat pipa palsu `/dev/tsc2007_2-0048`.
4. **`knobshim.so`** (Urutan Keempat, sebelum `audioshim.so`): Mendefinisikan variabel global data (`g_master_gain`) yang dibaca oleh `audioshim.so`.
5. **`audioshim.so`** (Urutan Kelima): Mendefinisikan variabel global data (`g_vu_peak`) yang dibaca kembali oleh `knobshim.so`.
