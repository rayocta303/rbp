# 22 — Kamus Data Teknis & Spesifikasi ABI `rbp` (Data Dictionary)

Dokumentasi komprehensif Kamus Data (*Data Dictionary*) untuk biner internal **Pioneer DJ XDJ-RX3 rekordbox standalone player (`rbp`)** versi firmware v1.20 (ARM32 soft-float).

Dokumen ini mencatat seluruh spesifikasi antarmuka biner (*Application Binary Interface* / ABI), pemetaan register CPU ARM, struktur data memori internal (*in-memory structs*), variabel tunggal (*global singletons*), kode tombol (*keycodes*), protokol input, dan saluran output hardware.

Sumber referensi utama kode:
*   [scripts/shims/rbp_abi.h](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/rbp_abi.h) — definisi alamat memori, konstanta, dan offset field
*   [scripts/shims/rbp_bridge.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/rbp_bridge.c) — jembatan pemanggilan fungsi C/C++ singleton
*   [tools/patch-rbp/PATCHES.md](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/tools/patch-rbp/PATCHES.md) — instruksi assembly dan modifikasi opcode biner
*   [tools/read_regs.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/tools/read_regs.c) — dump register ptrace 32-bit ARM compat

---

## 1. Karakteristik Biner & Lingkungan Eksekusi CPU

| Properti | Nilai / Spesifikasi Teknis | Keterangan |
|---|---|---|
| **Nama Biner** | `pdj/rbp` (disebut `rb` secara internal) | Biner pemutar utama standalone rekordbox dari firmware XDJ-RX3 v1.20 |
| **Arsitektur CPU** | **ARMv7-A 32-bit** (`armv7l`), Little-Endian | Dieksekusi pada kernel 64-bit (`AArch64`) melalui kompatibilitas `CONFIG_COMPAT` (32-bit EL0) |
| **Float ABI** | **Soft-Float** (`armel` / `gnueabi`) | Operasi floating point menggunakan emulasi software integer (bukan `VFP`/`NEON` hard-float) |
| **Format ELF** | **Non-PIE** (*Position Independent Executable* = FALSE) | Alamat virtual bersifat **absolut dan tetap**. Segment `PT_LOAD` executable mulai di `0x00008000`. |
| **Ukuran Memori** | `MemSiz = 0x56480b8` (mencapai `0x05b5acd4`) | Alamat `.bss` dan heap terpetakan pada rentang absolut yang sama di setiap instance proses |
| **Standard ABI** | ARM Architecture Procedure Call Standard (**AAPCS**) | Standar pemanggilan fungsi C/C++ (register `r0`-`r3` untuk argumen, `r0` return) |

---

## 2. Register CPU ARM32 (Register Dictionary)

Penggunaan register CPU pada titik masuk fungsi penting `rbp` (berdasarkan konvensi AAPCS dan tangkapan ptrace di [tools/read_regs.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/tools/read_regs.c)):

| Register | Nama Konvensi | Peran dalam Pemanggilan Fungsi `rbp` | Contoh Penggunaan Nyata |
|---|---|---|---|
| **`r0`** | `a1` / Argument 1 | • Pointer `this` pada method C++ class<br/>• Nilai kembalian fungsi (*Return Value*)<br/>• Argumen pertama (misal: ID kanal deck / status input) | Pada `sendKey()`: menampung pointer `KeyManager*`. Pada `getLedValue()`: menampung objek `MonoLvMeter*`. |
| **`r1`** | `a2` / Argument 2 | • Argumen kedua fungsi (misal: kode tombol / status boolean / pointer data) | Pada `setDeckQuantizing(ch, state)`: `r0=ch`, `r1=bool state`. Pada `onKey`: menampung struct `KeyInput*`. |
| **`r2`** | `a3` / Argument 3 | • Argumen ketiga fungsi (misal: operasi tombol `op`, nilai parameter, panjang data) | Pada injeksi `sendKey()`: `r2` membawa operasi `OP_PRESS` (0), `OP_REPEAT` (1), atau `OP_RELEASE` (2). |
| **`r3`** | `a4` / Argument 4 | • Argumen keempat fungsi<br/>• Register sementara pemuatan konstanta PC-relative literal pool | Digunakan untuk kalkulasi alamat global `.bss` sebelum instruksi `ldr` / `str`. |
| **`r4` – `r10`** | `v1` – `v7` / Variable | Register lokal callee-saved (disimpan ke stack saat masuk fungsi dengan `push {r4-rX, lr}`) | Menyimpan variabel perhitungan internal loop, pencarian index array `FixedAddressArray`, dan state playback. |
| **`r11` (`fp`)** | Frame Pointer | Penunjuk frame stack lokal fungsi (opsional tergantung optimasi compiler `-fomit-frame-pointer`) | Dipakai untuk backtracking stack frame saat terjadi fatal exception atau signal trace. |
| **`r12` (`ip`)** | Intra-Procedure Call | Register scratch perantara linker dan PLT trampoline stub | Dipakai saat memanggil fungsi dinamis antar library `libc.so` dan `libpthread.so`. |
| **`r13` (`sp`)** | Stack Pointer | Menunjuk puncak stack proses (wajib 8-byte aligned pada batas pemanggilan AAPCS) | Dialokasikan oleh kernel Linux pada rentang `0xbexxxxxx`. |
| **`r14` (`lr`)** | Link Register | **Alamat Kembali (*Return Address*)**. Sangat krusial pada hooking shim. | **Hook VU Meter**: Nilai `lr` pemanggil `getLedValue` membedakan channel:<br/>• `0x2d0940` = Master Meter<br/>• `0x2d0a44` = Deck 1 Meter<br/>• `0x2d0a8c` = Deck 2 Meter |
| **`r15` (`pc`)** | Program Counter | Menunjuk alamat instruksi yang sedang dieksekusi | Ditangkap oleh [crashcatch.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/crashcatch.c) via `ucontext_t` saat terjadi SIGSEGV/SIGBUS. |
| **`cpsr`** | Status Register | Bit status: Mode eksekusi (ARM 32-bit vs Thumb), bit interupsi, dan flag perbandingan NZCV | CPSR bit 5 (T bit) bernilai 0 menandakan mode eksekusi 32-bit ARM penuh (bukan Thumb). |

---

## 3. Kamus Alamat Memori Global & Struktur Data (.bss / Heap)

Struktur objek C++ dan pointer global yang diakses langsung oleh shim via dereferensi memori atau pemanggilan vtable:

### 3.1 Objek Manajer UI Utama (`IUiObjManager`)
Pointer Root Singleton: **`UI_OBJ_MGR_HOLDER = 0x026867C0`** (`*(uint32_t*)0x026867C0 = IUiObjManager*`)

```
IUiObjManager (0x026867C0)
 ├── +0x40  [uint32_t*]   Array pointer PlayerInnards (Kanal 1 & Kanal 2)
 ├── +0x48  [int]          Jumlah Player aktif (count = 2)
 ├── +0x50  [uint32_t*]   Pointer ke ui::Mixer (+80 dari base uiobj)
 │           └── +0xb8     [long**] Pointer level cached VU meter (MIXER_LVLOFF)
 ├── +0x64  [uint32_t*]   Pointer ke KeyManager (KEY_MANAGER_OFF = 100)
 │           └── Vtable[2] Fungsi sendKey() (SENDKEY_VTABLE_WORD = 2)
 ├── +0x68  [uint32_t*]   Pointer ke LedManager (LEDMGR_OFF_LEDSTAT = 104)
 │           └── +0x30     Tabel status uif::LedStat (LEDSTAT_OFF = 0x30)
 ├── +0xc4  [uint32_t*]   Array pointer UsbStorageManager (UIOBJMGR_OFF_MGRARR)
 └── +0xcc  [int]          Jumlah UsbStorageManager (UIOBJMGR_OFF_MGRNUM = 2)
```

### 3.2 Struktur `PlayerInnards` (Per-Deck Engine State)
Diakses melalui `player = *(uiobj + 0x40)[ch - 1]`:

| Offset | Tipe Data | Nama Konstanta | Deskripsi Fungsional |
|---|---|---|---|
| `+0x26` | `uint8_t` | `PLAYERINNARDS_CHAN_OFF` | Nomor channel deck (1 = Deck 1, 2 = Deck 2). |
| `+0x30` | `void*` | `PLAYERINNARDS_ENGINE_OFF` | Pointer ke objek `djengine::DjEngineIF` milik deck ini. |
| `+0x38` | `uint32_t` | `PLAYERINNARDS_PADCMD_OFF` | Mode eksekusi pad: `1`=Call, `2`=Record, `3`=Delete. Nilai `3` memicu `onHotCueDeleteEvent`. |
| `+0x74` | `uint8_t` | `PLAYERINNARDS_MODE_OFF` | Identifier struktur saat pemindaian vtable. |

### 3.3 Status Browser Layar & Navigasi (`uiBrowse`)
Alamat Tunggal BSS: **`UIBROWSE_GLOBAL = 0x0326f8b8`**

| Offset | Tipe Data | Nama Konstanta | Deskripsi & Nilai Legal |
|---|---|---|---|
| `+0x00` | `int32_t` | `UIBROWSE_OFF_MODE` | Mode layar aktif:<br/>• `1`: `BROWSE_MODE_PLAY` (Layar Performa / Waveform)<br/>• `7`: `BROWSE_MODE_UTILITY` (Menu Settings / Pengaturan Sistem)<br/>• `8`: `BROWSE_MODE_TRACKFILTER` (Pencarian Filter Lagu)<br/>• `10`: `BROWSE_MODE_SHORTCUT` (Layar Pintasan Cepat) |
| `+0x04` | `int32_t` | `UIBROWSE_OFF_DEVICE` | ID perangkat yang sedang di-browse (USB1, USB2, Rekordbox Link). |
| `+0x14` | `int32_t` | `UIBROWSE_OFF_ACTIVE_LIST` | `0` = mode navigasi list; `!= 0` = sedang mengedit nilai parameter. |
| `+0x18` | `int32_t` | `UIBROWSE_OFF_PLAY_SUBGATE` | Flag subgate layar performa: jika bernilai `1`, rotasi selector tidak mengubah zoom waveform. |
| `+0x6c` | `int32_t[2]`| `UIBROWSE_OFF_CURSOR` | Indeks baris kursor yang disorot (dibatasi 0..11 baris pada layar). |
| `+0xac` | `int32_t` | `UIBROWSE_OFF_CALIB_A` | Flag sub-layar kalibrasi touch UTILITY (kata A). |
| `+0xb8` | `int32_t` | `UIBROWSE_OFF_CALIB_B` | Flag sub-layar kalibrasi touch UTILITY (kata B). Jika keduanya non-zero, gesture UTILITY diblokir. |
| `+0x3b8`| `int32_t` | `UIBROWSE_OFF_ITEMNUM` | Jumlah total item pada list yang sedang aktif. |
| `+0x3bc`| `int32_t` | `UIBROWSE_OFF_INITIAL` | Indeks baris pertama list yang terlihat pada jendela viewport. |

### 3.4 Struktur Pengatur Efek (`BeatEffectManager` & `BeatEffect`)
Diakses via dereferensi: `MixerEngine (0x011493c0) -> +0x58 (BEM_IN_MIXER)`

```
MixerEngine (0x011493c0)
 └── +0x58  BeatEffectManager*
      ├── +0x00  [int]     Channel penugasan efek (0=Pl 1, 1=Pl 2, 5=MASTER)
      ├── +0x50  [int]     Tipe efek aktif (0=OFF, 1..14 jenis efek)
      └── +0x08  BeatEffect* (Objek efek yang sedang aktif)
           ├── +0x20  [float]    Level / Depth basah-kering (0.0f s/d 1.0f)
           ├── +0x24  [int32_t]  Waktu efek dalam milidetik (msec)
           ├── +0x28  [int32_t]  Batas waktu maksimum efek (time max)
           ├── +0x2c  [int32_t]  Batas waktu minimum efek (time min)
           ├── +0x3c  [uint8_t]  Status Efek ON / OFF (1 = Aktif, 0 = Nonaktif)
           └── +0x44  [int32_t]  Tingkat beat fraction ladder (posisi anak tangga 0..9)
```

### 3.5 Pengaturan Skala Waveform & Grid Adjust
*   **`WAVE_SCALE_GLOBAL = 0x0216bac8`**: Skala zoom gelombang visual (`0` s/d `4`). Nilai `4` = zoom terdekat (*zoom in*, 100 sampel/layar); `0` = zoom terjauh (*zoom out*, 1600 sampel/layar).
*   **`GRID_ADJUST_FLG_GLOBAL = 0x03254a9c`**: Mode beatgrid (`0` = rotasi selector melakukan zoom gelombang; `1` = rotasi menggeser posisi grid ketukan).

### 3.6 Tabel Status LED Panel (`uif::LedStat`)
Alamat Array: Diperoleh via `*(UI_OBJ_MGR_HOLDER) -> +104 -> +0x30`.
Setiap entri berukuran **`0x2c` byte (44 byte)**:

| Offset | Tipe Data | Nama Field | Deskripsi Nilai |
|---|---|---|---|
| `+0x00` | `uint32_t` | `ID` | ID fungsi kontrol hardware LED (misal: `49`=Play, `4`=Sync, `5`=Master, `3`=Vinyl, `6`=Master Tempo, `11`=Slip). |
| `+0x04` | `uint32_t` | `Channel` | Kanal deck kepemilikan LED (`1` = Deck 1, `2` = Deck 2). |
| `+0x10` | `uint32_t` | `State` | Status pencahayaan LED:<br/>• `0` = Mati (*Dark*)<br/>• `1` = Menyala konstan (*Solid ON*)<br/>• `2` = Berkedip (*Blinking*) |
| `+0x14` | `uint32_t` | `Unassigned` | `1` = Pad kosong / tidak memiliki cue point (harus diredupkan/dimatikan); `0` = Memiliki cue point aktif. |
| `+0x1c` | `uint32_t` | `BlinkPeriod` | Periode siklus kedip dalam milidetik (`ms`). Contoh: 500ms untuk pause cue, 250ms untuk filter. |

---

## 4. Kamus Input: Pioneer Keycode & Operasi ABI

Injeksi input hardware dilakukan melalui fungsi singleton `send_rx_key(channel, keycode, op, value, f_val)`:

### 4.1 Definisi Operasi Tombol (`op`)
*   **`OP_PRESS = 0`**: Tombol ditekan pertama kali (*Key Down*).
*   **`OP_REPEAT = 1`**: Tombol ditahan terus-menerus (*Key Hold / Repeat*). Wajib untuk membuka menu `K_TRACKFILTER` (0x420f) dan eksekusi `K_USBSTOP` (0x8002).
*   **`OP_RELEASE = 2`**: Tombol dilepas (*Key Up*).
*   **`OP_ROTATE = 4`**: Perputaran encoder rotari (jog wheel atau selector browser).
*   **`OP_VALUE = 5`**: Nilai kontrol analog mutlak (payload dikirimkan via float parameter `f_val`).

### 4.2 Daftar Lengkap Keycode RX3

#### Transport & Decks
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x4101` | `K_PLAY` | Tombol PLAY / PAUSE | Toggle status playback deck. LED Play berkedip saat mode pause. |
| `0x4102` | `K_CUE` | Tombol CUE Deck | Menetapkan temporary memory cue point saat pause; jump to cue saat play. |
| `0x4112` | `K_SYNC` | Tombol BEAT SYNC | Mengunci tempo dan phase grid ke deck Master. |
| `0x4111` | `K_MASTER` | Tombol MASTER Deck | Menetapkan deck aktif sebagai referensi Master tempo. |
| `0x4104` | `K_VINYL` | Mode JOG VINYL / CDJ | Mengubah mode respon jog wheel antara vinyl scratch dan CDJ pitch bend. |
| `0x4108` | `K_MT` | MASTER TEMPO (Key Lock) | Mengunci nada lagu (*musical pitch*) saat fader tempo digeser. |
| `0x4107` | `K_TEMPO_RANGE` | Rentang Skala Pitch | Siklus skala fader tempo: `±6%` → `±10%` → `±16%` → `WIDE`. |
| `0x4109` | `K_TEMPO_SLIDER`| Fader Kecepatan Pitch | `OP_VALUE`: Nilai float 0.0f s/d 1.0f (tengah = 0.5f). |
| `0x410b` | `K_QUANTIZE` | Kuantisasi Ketukan Deck | Menyelaraskan timing loop, cue, dan hot cue ke grid ketukan terdekat. |
| `0x410f` | `K_REV` | Reverse Playback | Memutar lagu ke arah mundur selama ditekan atau toggle. |
| `0x4110` | `K_SLIP` | Mode SLIP | Playback terus berjalan di latar belakang saat scratch, loop, atau reverse aktif. |

#### Jog Wheel
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x4306` | `K_JOG_TOUCH` | Sensor Sentuh Pelat Jog | `OP_PRESS` saat pelat atas disentuh; `OP_RELEASE` saat tangan diangkat. |
| `0x4305` | `K_JOG_ROT` | Putaran Roda Jog Wheel | `OP_ROTATE`: Mengirimkan kecepatan rotasi (`rev/s`) dan posisi delta. |

#### Performance Pads & Loop
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x4113` | `K_HOTCUE` | Pemilih Mode Pad: HOT CUE | Mengubah 8 pad fisik untuk memicu atau merekam hot cue instant. |
| `0x4114` | `K_ALOOP` | Pemilih Mode: AUTO BEAT LOOP | Mengubah 8 pad fisik untuk memicu beat loop otomatis (1/32 s/d 16 beat). |
| `0x4115` | `K_SLIPLOOP` | Pemilih Mode: SLIP LOOP | Memicu loop gulung saat pad ditekan dan kembali ke lagu saat dilepas. |
| `0x4116` | `K_BEATJUMP` | Pemilih Mode: BEAT JUMP | Menggeser posisi kursor pemutaran maju/mundur sesuai fraksi beat. |
| `0x4117` | `K_PAD1` | Performance Pad 1 (s/d Pad 8)| Pemicu fungsi pad terpilih (Pad 1 = `0x4117`, Pad 2 s/d 8 berurutan). |
| `0x410c` | `K_LOOPIN` | Tombol Loop In (Manual) | Menetapkan titik awal loop manual / potong 4-beat loop. |
| `0x410d` | `K_LOOPOUT` | Tombol Loop Out (Manual) | Menetapkan titik akhir loop manual dan memulai pengulangan. |
| `0x410e` | `K_RELOOP` | Tombol Reloop / Exit | Keluar dari loop aktif atau melompat kembali ke loop terakhir. |

#### Mixer & Sound Color FX
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x5019` | `K_TRIM` | Gain / Trim Channel | `OP_VALUE`: Penguatan pre-fader per kanal. |
| `0x501a` | `K_EQH` | Equalizer Frekuensi Tinggi | `OP_VALUE`: Nada High per kanal (0..1023, center = 512). |
| `0x501b` | `K_EQM` | Equalizer Frekuensi Menengah| `OP_VALUE`: Nada Mid per kanal. |
| `0x501c` | `K_EQL` | Equalizer Frekuensi Rendah | `OP_VALUE`: Nada Low / Bass per kanal. |
| `0x501e` | `K_FADER` | Fader Volume Channel | `OP_VALUE`: Posisi fader volume vertikal kanal. |
| `0x6017` | `K_XFADER` | Crossfader Horizontal | `OP_VALUE`: Pembagi rasio audio Deck 1 dan Deck 2. |
| `0x4403` | `K_MASTERLVL` | Master Output Volume Level | `OP_VALUE`: Knob volume utama output master RCA/XLR. |
| `0x4405` | `K_HPMIX` | Headphone Mixing (Cue/Master)| `OP_VALUE`: Rasio pencampuran audio cue monitor dan output master. |
| `0x4406` | `K_HPLEVEL` | Headphone Volume Level | `OP_VALUE`: Potensiometer pengatur kenyaringan headphone jack. |
| `0x4407` | `K_MASTERCUE` | Tombol Cue Master Headphone| Mengaktifkan sinyal Master ke bus headphone monitor. |
| `0x509d` | `K_COLOR` | Knob Sound Color FX | `OP_VALUE`: Posisi filter/efek warna (tengah = netral). |
| `0x50a6` | `K_FILTER` | Tombol Sound Color: Filter | Memilih tipe filter HPF / LPF. |
| `0x50a2` | `K_DUBECHO` | Tombol Sound Color: Dub Echo| Memilih tipe efek delay dub feedback. |
| `0x50a3` | `K_SWEEP` | Tombol Sound Color: Sweep | Memilih efek sapuan modulasi nada. |
| `0x50a4` | `K_NOISE` | Tombol Sound Color: Noise | Memilih generator white noise tertapis. |
| `0x50a5` | `K_SPACE` | Tombol Sound Color: Space | Memilih efek reverb ruang angkasa. |
| `0x50a1` | `K_CRUSH` | Tombol Sound Color: Crush | Memilih efek distorsi bitcrusher. |

#### Beat FX
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x448b` | `K_BFXTYPE` | Pemilih Jenis Beat FX | Siklus jenis efek (Delay, Echo, Reverb, Flanger, dll.). |
| `0x448c` | `K_BFXCH` | Channel Assign Beat FX | Menetapkan target efek (`1`=Ch 1, `2`=Ch 2, `5`=`BFX_CH_MASTER`). |
| `0x448d` | `K_BFX` | Tombol Aktivasi Beat FX | Toggle ON / OFF efek beat. |
| `0x448e` | `K_TIME` | Kenop Beat FX Time | Parameter durasi milidetik delay/echo. |
| `0x448f` | `K_DEPTH` | Kenop Beat FX Level/Depth | `OP_VALUE`: Intensitas Wet/Dry efek (skala float 0.0f s/d 1.0f). |
| `0x4490` | `K_BEATPREV` | Fraksi Beat Lebih Pendek | Mengurangi durasi fraksi beat menjadi setengah (misal: 1 beat → 1/2 beat). |
| `0x4491` | `K_BEATNEXT` | Fraksi Beat Lebih Panjang | Menggandakan durasi fraksi beat (misal: 1 beat → 2 beat). |
| `0x4492` | `K_TAP` | Tombol BPM Tap Manual | Menghitung tempo lagu manual berdasarkan interval ketukan jari. |

#### Navigasi & Tampilan Browser
| Keycode | Nama Simbolis | Deskripsi Fungsi | Parameter & Perilaku |
|---|---|---|---|
| `0x420c` | `K_SELECTOR` | Rotari Selector Browse | `OP_ROTATE`: Menggeser baris kursor lagu naik (+1) atau turun (-1). |
| `0x4311` | `K_LOAD` | Tombol Pemuat Lagu (LOAD) | Memuat track yang disorot ke dalam engine playback deck target. |
| `0x420d` | `K_BACK` | Tombol Kembali (BACK) | Melangkah kembali satu tingkat hierarki folder/kategori browser. |
| `0x0202` | `K_BROWSE` | Tombol BROWSE View | Membuka antarmuka pencarian pustaka lagu. |
| `0x0201` | `K_SOURCE` | Tombol SOURCE View | Membuka pemilihan sumber input media (USB 1, USB 2, Link). |
| `0x0209` | `K_USB1` | Akses Langsung USB 1 | Pintasan langsung ke katalog flashdisk USB slot 1. |
| `0x0207` | `K_LINK` | Akses Sumber PRO DJ LINK | Menampilkan perangkat Pro DJ Link yang terhubung di jaringan. |
| `0x0208` | `K_REKORDBOX` | Akses Mode Rekordbox | Menampilkan sumber koneksi software rekordbox komputer/ponsel. |
| `0x0203` | `K_TAGLIST` | Antarmuka TAG LIST | Membuka daftar tampungan lagu yang ditandai untuk sesi tampil. |
| `0x0204` | `K_PLAYLIST` | Antarmuka PLAYLIST | Menampilkan folder daftar putar lagu (*playlist bank*). |
| `0x0205` | `K_SEARCH` | Antarmuka PENCARIAN Teks | Membuka keyboard visual layar sentuh untuk mencari judul lagu. |
| `0x0206` | `K_MENU` | Tombol Menu & Pengaturan | Toggle panel My Settings / Utility. |
| `0x020b` | `K_INFO` | Tombol Layar INFO Track | Menampilkan metadata rinci, artwork besar, dan key lagu. |
| `0x0210` | `K_SHORTCUT` | Jendela Pintasan Cepat | Menampilkan tombol cepat kecerahan layar, waveform dual, dll. |
| `0x420f` | `K_TRACKFILTER`| Panel Filter Kriteria Lagu | **Wajib `OP_REPEAT`**: Memfilter lagu berdasarkan BPM, Key, dan Tag. |
| `0x8002` | `K_USBSTOP` | Tombol Eject Aman USB | **Wajib `OP_PRESS` lalu `OP_REPEAT`**: Melepas drive USB dengan aman. |

---

## 5. Kamus Input: Protokol Perangkat Keras Eksternal

### 5.1 Paket Protokol Sentuh Resistif TI TSC2007
Disimulasikan oleh [tscfake.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/tscfake.c) melalui pipa biner semu `/dev/tsc2007_2-0048`:

| Posisi Byte | Tipe Data | Nama Field | Deskripsi Nilai |
|---|---|---|---|
| `Byte 0` | `uint8_t` | `flag` | Status sentuhan jari: `0x01` = Tekan (*Pen Down*), `0x00` = Lepas (*Pen Up*). |
| `Byte 1` | `uint8_t` | `pad` | Byte padding penyeimbang alignment memori biner (selalu `0x00`). |
| `Byte 2-3` | `uint16_t`| `x` | Koordinat X layar (Big-Endian): rentang 0 s/d 1279 pixel. |
| `Byte 4-5` | `uint16_t`| `y` | Koordinat Y layar (Big-Endian): rentang 0 s/d 799 pixel. |

### 5.2 Protokol Perangkat Blok USB Flashdisk
Dipantau oleh [usb-watch.sh](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/device/usb-watch.sh):

| Parameter | Spesifikasi | Keterangan |
|---|---|---|
| **Path Perangkat** | `/dev/sd[a-z][1-9]` | Partisi blok yang terdeteksi via `/sys/block/sd*` pada bus USB. |
| **Berkas Wajib** | `PIONEER/rekordbox/export.pdb` | Flashdisk diabaikan jika tidak memiliki database rekordbox valid. |
| **Format Filesystem** | `vfat` / `exfat` / `hfsplus` | Mount options: `flush,rw,noatime,dmask=000,fmask=000,codepage=437,utf8`. |
| **Bind Mount Titik Chroot** | `/media/usb1/sda1` & `/media/usb2/sda1` | Dipetakan langsung ke dalam hierarki direktori chroot soft-float. |
| **Sinyal IPC FIFO** | `/tmp/udev_usb1` & `/tmp/udev_usb2` | Format string teks perintah: `"mount /media/usb1/sda1\n"`. |

---

## 6. Kamus Output: Audio, Display, LED & State Bersama

### 6.1 Spesifikasi Aliran Audio PCM ALSA Output
Dikelola oleh [audioshim.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/audioshim.c) dan [s24pack.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/s24pack.c):

| Saluran Output | Format PCM | Kanal Audio | Kecepatan Sampling | Periode & Latensi |
|---|---|---|---|---|
| **Koleksi Internal `rbp`** | `SND_PCM_FORMAT_S24_LE` | 3 Aliran Stereo Terpisah: Stream 0 (Master), 1 (Phone), 2 (Booth) | 44.100 Hz (44.1 kHz) | 64 frame / periode (~1.45 ms buffer period) |
| **Hardware DDJ-FLX4** | `SND_PCM_FORMAT_S24_3LE` | **4 Kanal Interleaved**:<br/>• Kanal 1-2: Master Output (RCA)<br/>• Kanal 3-4: Headphone Output (Jack 3.5mm) | 44.100 Hz (Natif) | 64 frame / blok tulis (`768 byte` per eksekusi `snd_pcm_writei`) |
| **Mirror HDMI Monitor** | `SND_PCM_FORMAT_IEC958_SUBFRAME_LE` | 2 Kanal Stereo (Master Mirror) | 44.100 Hz | Ditulis non-blocking ke subframe HDMI chip bcm2835 |

### 6.2 Spesifikasi Framebuffer Grafis Layar
Dikelola oleh [fb_shim.c](file:///p:/XDJ-Firmware/XDJ-RX3-Amlogic-S905X/scripts/shims/fb_shim.c) dan driver DirectFB:

| Atribut Tampilan | Parameter Konfigurasi | Keterangan |
|---|---|---|
| **Node Framebuffer** | `/dev/fb0` (`vc4drmfb`) | Driver emulasi DRM fbdev kernel Raspberry Pi OS. |
| **Resolusi Geometri** | **1280 × 800 pixel** (Landscape) | Resolusi natif antarmuka pengguna `rbp` (rasio aspek 16:10). |
| **Format Kedalaman Warna** | **16 bit-per-pixel (RGB565)** | Byte format: 5-bit Red, 6-bit Green, 5-bit Blue (`line_length = 2560 byte`). |
| **Pacing Laju Segar** | **60 FPS** (16.67 milidetik/frame) | Diatur oleh pacer buatan pada `fb_shim.c` guna mencegah render thread membakar 100% CPU. |

### 6.3 State Bersama Antar-Pustaka (`shmstate.h`)
Komunikasi data berkecepatan tinggi antara pustaka `knobshim.so` dan `audioshim.so`:

| Variabel Global C | Tipe Data | Modul Penulis | Modul Pembaca | Definisi Fungsi |
|---|---|---|---|---|
| **`g_master_gain`** | `float` (0.0f s/d 1.0f) | `knobshim.so` (Kenop Master Level) | `audioshim.so` (Pengali Gain) | Skala volume master sebelum sampel dikirim ke output DAC. |
| **`g_vu_peak`** | `float[3]` | `audioshim.so` (Detektor Peak) | `knobshim.so` (Generator MIDI VU) | Nilai puncak level audio untuk Master, Deck 1, dan Deck 2. |
