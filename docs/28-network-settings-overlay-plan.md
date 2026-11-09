# Perencanaan Fitur: OS Network Settings Overlay (Wi-Fi & Bluetooth) di Tampilan RBP

Dokumen ini merinci arsitektur, desain visual, antarmuka sentuh, integrasi OS Linux, dan tahapan implementasi fitur **Pengaturan Wi-Fi & Bluetooth Level OS di Tampilan RBP** (Network Settings Overlay) dan **Tombol Strip Wi-Fi & Bluetooth pada Top Bar** untuk Pioneer XDJ-RX3 yang berjalan pada Amlogic S905X (Armbian Linux).

---

## 1. Analisis Kebutuhan & Konsep Dasar

### A. Karakteristik Bawaan `rbp` (XDJ-RX3)
- Software pemutar Pioneer Rekordbox Player (`/root/pdj/rbp`) dirancang untuk hardware asli XDJ-RX3 yang **tidak memiliki kartu jaringan Wi-Fi maupun Bluetooth** (hanya kabel Ethernet Pro DJ Link dan USB).
- Di dalam kode binary `rbp`, **sama sekali tidak ada view, halaman, objek GUI, maupun menu untuk konfigurasi Wi-Fi / Bluetooth**. Menu Utility bawaan hanya menyediakan setting IP statis/DHCP kabel LAN.

### B. Kebutuhan Pengguna pada Platform Amlogic S905X
- Mesin Amlogic S905X menjalankan **Armbian Linux** lengkap yang memiliki hardware adaptor Wi-Fi (`wlan0`) dan Bluetooth (`hci0`), serta didukung oleh stack jaringan `NetworkManager` (`nmcli`), `rfkill`, dan `bluez`.
- Pengguna ingin mengendalikan konektivitas OS ini **langsung dari antarmuka visual RBP di layar sentuh unit / VNC** tanpa perlu keluar ke terminal SSH atau membuka desktop Linux terpisah.
- Solusi: **Layering Overlay Shims (`fbshim.so`)**:
  1. Menambahkan **Tombol Strip Wi-Fi & Bluetooth** di baris atas (Top Bar) sebagai indikator status sekaligus tombol pembuka.
  2. Menampilkan **Halaman Pengaturan Jaringan OS** ("DISINI PENGATURAN JARINGAN") sebagai layer di atas RBP (rentang `y: 50..710`), sehingga pengguna dapat scan Wi-Fi, memasukkan password via virtual keyboard, menghubungkan hotspot, dan mengatur Bluetooth langsung dari layar sentuh konsol.

---

## 2. Desain Visual & Tata Letak Antarmuka (UI Layout)

### A. Strip Tombol Wi-Fi & Bluetooth pada Top Bar
Berdasarkan referensi visual:
```
+--------------------------------------------------------------------------------------------------+
| REMAIN 00:00:00 [======== Progress Bar ========]    [ (•) Wi-Fi  * BT ]  00:00:00  [< INFO ]    |  y: 0..50
+--------------------------------------------------------------------------------------------------+
```

1. **Koordinat & Dimensi Strip Background**:
   - **X**: `740` hingga `830` (lebar 90 piksel).
   - **Y**: `6` hingga `44` (tinggi 38 piksel, terpusat vertikal pada Top Bar 50px).
   - **Visual**: Rounded pill box (radius sudut 6px) dengan warna latar abu-abu gelap transparan (`#1b1e23` / ARGB `0xD01b1e23`), garis tepi halus (`#32373f`).
2. **Ikon Wi-Fi**:
   - Posisi: `x = 750..778`, `y = 13..37` (dimensi 24x24px).
   - **Status Terhubung**: Putih terang (`#ffffff`) dengan busur sinyal menyala hijau/putih.
   - **Status Terputus / Off**: Abu-abu redup (`#555a64`) dengan garis diagonal (strike-through).
3. **Ikon Bluetooth**:
   - Posisi: `x = 790..818`, `y = 13..37` (dimensi 24x24px).
   - **Status Terhubung / Aktif**: Ikon Rune Bluetooth biru terang khas (`#0082fc`) atau putih terang.
   - **Status Nonaktif**: Abu-abu redup (`#555a64`).
4. **Interaksi Touch**:
   - Area hit-test: `x: 735..835`, `y: 0..50`.
   - Sentuhan (tap) pada area ini akan meng-toggle buka/tutup halaman overlay jaringan.
   - Sentuhan langsung dikonsumsi (`return 1`), sehingga `rbp` di bawahnya tidak menerima klik liar.

---

### B. Halaman Overlay Pengaturan Jaringan (Network Settings Overlay)
Berdasarkan referensi visual:
```
+--------------------------------------------------------------------------------------------------+
| TOP BAR: STATUS / TITLE                              [ (•) Wi-Fi  * BT ]  00:00:00  [< INFO ]    |  y: 0..50
+--------------------------------------------------------------------------------------------------+
| ================================= NETWORK SETTINGS ========================================= [X] |  y: 50..110
|                                                                                                  |
|  [TAB: WI-FI]          [TAB: BLUETOOTH]         [TAB: HOTSPOT / AP]      [TAB: STATUS & IP]      |  y: 110..160
| ------------------------------------------------------------------------------------------------ |
|  Wi-Fi Radio: [ ON / OFF Toggle ]               Signal: 100%   IP: 192.168.137.22                |  y: 160..210
|  Connected: NOVA_7F08 (WPA2)                   Gateway: 192.168.137.1                            |
| ------------------------------------------------------------------------------------------------ |
|  Available Networks (Daftar Wi-Fi):                             [ Refresh / Scan ]               |  y: 210..250
|  +--------------------------------------------------------------------------------------------+  |
|  | (*) NOVA_7F08                   [Connected]                    Signal: 100% [Lock] [Forget]|  |  y: 250..310
|  | ( ) ALBADR                      [Available]                    Signal:  70% [Lock] [Connect|  |  y: 310..370
|  | ( ) Studio_Guest                [Available]                    Signal:  55% [Lock] [Connect|  |  y: 370..430
|  +--------------------------------------------------------------------------------------------+  |
|                                                                                                  |
|  Web Remote: http://unx.local:5800  atau  http://192.168.137.22:5800                             |  y: 650..700
+--------------------------------------------------------------------------------------------------+
| (( DECK 1 )) TIME: 00:00           BPM: 130.0   | (( DECK 2 )) REMAIN: 00:00         BPM: 130.0  |  y: 710..800
+--------------------------------------------------------------------------------------------------+
```

1. **Rentang Layar Overlay**:
   - `x`: `0` hingga `1280`.
   - `y`: `50` hingga `710` (tinggi 660px).
   - Area Deck Bawah (`y = 710..800`) dibiarkan tetap terlihat agar DJ tetap memantau status deck saat mengatur koneksi.
2. **Komponen Halaman**:
   - **Header Bar**:
     - Judul: "NETWORK SETTINGS" (warna aksen cyan `#00a2ff`).
     - Tombol Tutup `[ X ]` di pojok kanan atas (`x: 1210..1265, y: 58..102`).
   - **Tab Kategori**:
     - `Wi-Fi`: Pemindaian SSID, koneksi password, status link.
     - `Bluetooth`: Scanning perangkat, pairing controller / wireless audio.
     - `Hotspot / AP`: Mengaktifkan mode hotspot independen jika tidak ada router eksternal.
     - `System / IP`: Ringkasan antarmuka `eth0`, `wlan0`, status WebVNC server.
   - **Proteksi Sentuh (Touch Fence)**:
     - Selama overlay aktif, seluruh sentuhan pada rentang `y: 50..710` ditangkap oleh handler shims dan tidak dibocorkan ke `rbp`.

---

## 3. Arsitektur Teknis Implementasi

```mermaid
flowchart TD
    subgraph DirectFB / Framebuffer Pipeline
        VSYNC[VSync FBIOPAN_DISPLAY in fb_shim.c] --> DRAW[menu_draw.c]
        DRAW --> NET_DRAW[net_overlay_draw: Top Bar Strip & Overlay Panel]
        NET_DRAW --> FB[Shadow Buffer /dev/fb0]
    end

    subgraph Touch Event Pipeline
        TOUCH[Pointsrc / Evdev Touch Handler] --> NET_TOUCH{net_overlay_touch?}
        NET_TOUCH -- Yes (Consumed) --> ACTION[Toggle Open / Change Tab / Connect Wi-Fi]
        NET_TOUCH -- No (Bypass) --> RBP_TOUCH[Forward to Pioneer rbp GUI]
    end

    subgraph Backend Network Daemon
        ACTION --> SOCK[/tmp/net_cmd.sock]
        SOCK --> NET_SVC[net_daemon.py / background thread]
        NET_SVC --> NMCLI[nmcli / rfkill / ip]
        NMCLI --> STATE[/tmp/net_state.json cache]
        STATE --> NET_DRAW
    end
```

### A. Modul C di `scripts/shims/`:
1. **`net_overlay.h` & `net_overlay.c`**:
   - Mengelola state overlay (`g_net_overlay_open`, tab aktif, daftar Wi-Fi cache).
   - Menggambar strip tombol Top Bar (Wi-Fi + BT pill).
   - Menggambar panel overlay halaman penuh dengan antarmuka teks/kotak.
   - Menangani hit-test koordinat touch sentuh (tombol strip, tab, tombol refresh, tombol connect, tombol close).
2. **`net_icons.h`**:
   - Data bitmap ikon 24x24 untuk Wi-Fi (gelombang sinyal 100%, 75%, 50%, disconnected) dan Bluetooth (logo rune).
3. **`ui_overlay.c` / `menu_draw.c`**:
   - Memanggil `net_overlay_draw(&fv)` pada setiap render frame vsync.
4. **`pointsrc.c`**:
   - Mengarahkan touch event pertama ke `net_overlay_touch(down, x, y)`.

### B. Modul Backend Helper (Asynchronous Non-blocking):
- **Masalah**: Render thread DirectFB berjalan pada 30–60 FPS. Menjalankan perintah shell (`nmcli dev wifi list`) secara synchronous di dalam render loop akan menyebabkan frame drop / stutter 1-2 detik.
- **Solusi**:
  - Worker thread ringan atau script daemon background (`/opt/rblive4/scripts/net_daemon.py`):
    - Melakukan polling periodik setiap 2–3 detik untuk memeriksa status IP, SSID aktif, dan signal dBm.
    - Menulis data hasil scan ke shared memory atau file status ringkas `/tmp/net_state.json`.
    - Shims C di `net_overlay.c` membaca data yang sudah dicache di memori, sehingga render tetap 60 FPS tanpa jeda (0 ms blocking).

---

## 4. Tahapan Kerja (Roadmap Implementasi)

| Tahap | Aktivitas | Target Luaran |
|---|---|---|
| **Fase 1** | **Pembuatan Asset Ikon & Top Bar Strip** | - Membuat `net_icons.h` (bitmap icon Wi-Fi & BT 24x24).<br>- Menambahkan fungsi gambar strip pill di `net_overlay.c`.<br>- Tes render strip pada Top Bar di posisi `x: 740..830, y: 6..44`. |
| **Fase 2** | **Interaksi Touch Tombol Strip & Toggle Layar** | - Menambahkan hit-test tap pada area strip di `pointsrc.c`.<br>- State toggle buka/tutup overlay.<br>- Blokir klik ke `rbp` saat overlay terbuka. |
| **Fase 3** | **Render Halaman Overlay "PENGATURAN JARINGAN"** | - Menggambar background panel gelap `y = 50..710`.<br>- Header title dan tombol Close `[X]`.<br>- Tab bar: Wi-Fi, Bluetooth, Info IP & WebVNC.<br>- Verifikasi visual di layar VNC http://unx.local:5800/. |
| **Fase 4** | **Integrasi Backend Network (nmcli & rfkill)** | - Integrasi pembacaan status jaringan nyata dari Linux Armbian S905X (`wlan0`, SSID aktif, IP).<br>- Daftar SSID hasil scan interaktif.<br>- Toggle radio Wi-Fi & Bluetooth. |
| **Fase 5** | **Pengujian & Finalisasi** | - Verifikasi koneksi Wi-Fi tanpa stuttering pada audio/waveform.<br>- Commit & dokumentasi lengkap. |

---

## 5. Keputusan Desain & Keamanan Sistem

1. **Non-Intrusive**: Tidak memodifikasi binary `/root/pdj/rbp` milik Pioneer, seluruh visual overlay dan tombol dibuat di layer interposer `fbshim.so`.
2. **Deck Area Protection**: Area bawah deck (`y = 710..800`) tetap aktif dan terlihat, sehingga indikator playback lagu tetap termonitor.
3. **Zero Stuttering**: Semua operasi jaringan berat dieksekusi di background thread agar latensi audio ALSA dan rendering DirectFB tetap rock-solid.
