# 28 — Network Settings Overlay & Top Header Mod Navigation

Dokumentasi arsitektur, desain visual, antarmuka sentuh, integrasi OS Linux, dan tata letak fitur **Pengaturan Jaringan OS (Wi-Fi & Bluetooth)**, **Tombol Mod pada Top Header Bar (`KEY`, `STEMS`, `RMX`, `MODS`)**, serta **Card Theme Toggle pada Layar SHORTCUT** untuk proyek **RBP-Amlogic-S905X**.

---

## 1. Latar Belakang & Konsep Arsitektur

### A. Masalah pada Hardware XDJ-RX3 Bawaan
- Firmware Pioneer XDJ-RX3 (`/root/pdj/rbp`) dirancang untuk konsol fisik yang hanya memiliki port Ethernet Pro DJ Link (tanpa modul Wi-Fi atau Bluetooth internal).
- Di dalam kode binary `rbp`, tidak ada antarmuka grafis untuk scanning Wi-Fi, pairing Bluetooth, atau status jaringan nirkabel.

### B. Keunggulan Platform Amlogic S905X
- Perangkat TV Box dan SBC Amlogic S905X yang menjalankan **Armbian aarch64** memiliki chip nirkabel terintegrasi (Wi-Fi 802.11 b/g/n/ac dan Bluetooth 4.x/5.x).
- Pengguna membutuhkan kendali konektivitas nirkabel dan mod performance langsung dari layar sentuh konsol tanpa harus keluar ke shell terminal.
- Solusi diimplementasikan melalui **DirectFB Framebuffer Interposer (`fbshim.so`)**:
  1. **Top Header Bar Mod & Network Navigation** (`y: 8..44`): Menempatkan tombol navigasi mod dan indikator jaringan di baris atas layar, menjaga area tombol native **ZOOM, GRID, STATUS & BEAT FX** tetap bersih dan tidak terhalang.
  2. **Card Theme Toggle di Layar SHORTCUT**: Menimpa kartu unpopulated **JOG LCD** (`x: 792..1259, y: 89..198`) dengan tombol toggle **THEME** (`[ DARK ]` dan `[ LIGHT ]`) yang selaras 100% dengan estetika native Pioneer.
  3. **Halaman Overlay Jaringan OS (5 Tab)**: Modal dialog pada rentang `y: 50..579` untuk scanning Wi-Fi, info hotspot, status modul mod, dan restart stack player.

---

## 2. Tata Letak Top Header Bar (`y: 8..44`)

Pada layar utama (Waveform Performance View), area atas layar dialokasikan secara presisi:

```
+──────────────────────────────────────────────────────────────────────────────────────────────────────────+
| TRACK TITLE / ARTIST        | KEY | STEMS | RMX | MODS | [ (•) Wi-Fi  * BT ] | 00:00:00 [< INFO ]        |  y: 0..50
+──────────────────────────────────────────────────────────────────────────────────────────────────────────+
```

### Koordinat & Fungsi Tombol:
1. **`KEY` Button** (`x: 580..655, y: 8..44`):
   - Menampilkan label teks putih tajam dengan divider garis tipis.
   - Tap memicu pemilihan pad row Harmonic Mixing / Keyshift pada `librx3_core.so`.
2. **`STEMS` Button** (`x: 665..750, y: 8..44`):
   - Tap memicu pemilihan pad row Stems separation (Vocal, Drums, Inst).
3. **`RMX` Button** (`x: 760..835, y: 8..44`):
   - Tap mengaktifkan panel atau mode RMX-1000 Roll.
4. **`MODS` / `FX MOD` Button** (`x: 845..925, y: 8..44`):
   - Tap membuka langsung Tab 3 (MOD MENU) pada overlay dialog.
5. **Wi-Fi & Bluetooth Strip** (`x: 939..1066, y: 8..44`):
   - Menampilkan glif anti-aliased Wi-Fi (28×22) dan Bluetooth (16×24).
   - **Tanpa background box biru**: Mengikuti warna dasar top bar native secara transparan.
   - Sentuhan memberikan umpan balik warna cyan/biru elektrik sesaat pada ikon (`COL_ACCENT_HOV`).
   - Otomatis disembunyikan saat menu geser atas (Swipe-Down Menu) aktif, mencegah tumpang-tindih dengan tombol `MENU`.

---

## 3. Card Theme Toggle pada Layar SHORTCUT (`browse_mode == 10`)

Pada hardware XDJ-RX3, kartu kanan atas layar SHORTCUT menampilkan pengaturan **JOG LCD** yang tidak digunakan saat menggunakan kontroler USB. Kartu ini ditimpa secara mulus di memori framebuffer oleh `ui_overlay.c`:

### Koordinat & Tampilan Visual:
* **Area Kartu:** `x: 792..1259`, `y: 89..198` (lebar 468px, tinggi 110px).
* **Top Accent Bar:** Garis aksen amber/emas (`y: 89..91`, RGB `#633010`).
* **Latar Belakang & Border:** Background abu-abu gelap `#212021` dan garis tepi `#393c39`.
* **Judul:** Teks `THEME` di koordinat `x: 812, y: 108` warna abu-abu `#949694`.
* **Garis Pembatas:** Horizontal divider di `y: 138`.
* **Tombol Segmented (`y: 144..190`):**
  - **`[ DARK ]`** (`x: 812..1015`)
  - **`[ LIGHT ]`** (`x: 1035..1238`)
* **Styling State:**
  - Mode Aktif: Border menyala cyan `#00a0ff`, background biru gelap `#082839` (atau putih terang), dan teks putih tebal.
  - Mode Inaktif: Background `#181818`, border `#393c39`, teks abu-abu redup `#949694`.
* **Interaksi Sentuh:**
  - Tap pada `[ DARK ]`: Jika sedang dalam mode terang, otomatis memicu toggle kembali ke dark mode.
  - Tap pada `[ LIGHT ]`: Jika sedang dalam mode gelap, memicu sekuens key native `SHIFT + SHORTCUT` untuk mengaktifkan tema putih terang (`theme-white`).

---

## 4. Modal Overlay Jaringan OS (Rentang `y: 50..579`)

Ketika strip Wi-Fi / Bluetooth atau tombol `MODS` ditekan, modal overlay terbuka pada area tengah layar (`y: 50..579`), membiarkan waveform dan deck area (`y > 580`) tetap aktif:

### Struktur Tab:
* **Tab 0 — WI-FI:** Status koneksi SSID, IP address wlan0, tombol Scan jaringan, dan daftar access point yang dapat dipilih langsung.
* **Tab 1 — BLUETOOTH:** Toggle on/off modul Bluetooth, status adapter, dan scanning perangkat audio.
* **Tab 2 — HOTSPOT (AP):** Konfigurasi Wi-Fi Access Point mandiri (`XDJ-RX3-DIRECT`) untuk koneksi Rekordbox iPad atau remote control tanpa router eksternal.
* **Tab 3 — MOD MENU:** Status ringkasan modul performa (`core`, `stems`, `theme-white`, `keyshift`, `beatjump`, `now-playing`) serta tombol **RESTART RBP** yang terintegrasi langsung dengan perintah `systemctl restart rblive4`.
* **Tab 4 — STATUS / IP:** Rangkuman konfigurasi jaringan (wlan0 IP, eth0 IP, hostname, link rate).

---

## 5. Alur Data & Eksekusi

```
Sentuhan Jari (Touchscreen)
     │
     ▼
evdev (/dev/input/event*)
     │
     ▼
fbshim.so (net_zone.c / ui_overlay.c)
     │
     ├── Jika touch pada Top Header (KEY / STEMS):
     │   └─► tscfake_emit() mensimulasikan tap koordinat pad row
     │
     ├── Jika touch pada Top Header (MODS):
     │   └─► net_open(NET_TAB_MODS) membuka modal Mod Menu
     │
     ├── Jika touch pada Top Strip (Wi-Fi/BT):
     │   └─► net_open(NET_TAB_WIFI) membuka modal pengaturan jaringan
     │
     ├── Jika touch pada SHORTCUT Theme Card:
     │   └─► ov_theme_toggle() mengirim event SHIFT+SHORTCUT ke engine tema
     │
     └── Jika touch pada area kontrol native lainnya:
         └─► Dilewatkan 100% tanpa modifikasi ke rbp
```
