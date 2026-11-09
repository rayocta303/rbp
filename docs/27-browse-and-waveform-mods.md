# 27-browse-and-waveform-mods.md -- Planning & Technical Design for XDJ-RX3 Browse & Waveform Mods

## 1. Executive Summary & Context

Modifikasi pada seri XDJ (seperti pada mod XDJ-AZ di `xdj-az-mods-main`) menghadirkan kenyamanan alur kerja DJ profesional, seperti:
1. **Touch Preview / Song Info Remember**: Status panel Touch Preview dan Song Info tersimpan secara persisten dan tidak reset saat player di-reboot.
2. **Custom Columns & Row Formatting**: Kustomisasi tampilan baris track di browser (menampilkan Artwork, nomor track, Camelot Key, BPM, atau metadata sekunder seperti Release Year/Genre).
3. **Camelot Key / Mixed-In-Key Display**: Memaksa notasi Key selalu dalam format Alphanumeric Camelot (`8A`, `11B`, dsb.) di seluruh daftar lagu.
4. **60 FPS Waveform Presentation**: Animasi waveform dan rendering UI yang mulus pada refresh rate 60 Hz.

### Perbedaan Fundamental Arsitektur: XDJ-AZ vs XDJ-RX3
- **XDJ-AZ**: Menjalankan aplikasi 64-bit ARM (`EP147`, AArch64) dengan **JUCE C++ Multimedia Framework**. Mod AZ bekerja dengan me-rewrite instruksi AArch64 di memori (`mov w1, #33` -> `#16` pada `LinuxRepaintManager`) serta hooking vtable C++ JUCE (`juce::TableHeaderComponent::setColumnVisible`).
- **XDJ-RX3**: Menjalankan binary 32-bit ARM (`/root/pdj/rbp`, ARMv7l EABI5) berbasis **DirectFB 1.4 + Vivante G2D**. Binary `rbp` pada RX3 **tidak di-strip** (`36.708` simbol fungsi dan data global tersedia lengkap), menggunakan engine gambar milik Pioneer (`ui_com_draw_*` dan `ui_ListBrowse_*`), serta memiliki 12 baris track statis per layar (resolusi 1280x800).

Oleh karena itu, implementasi pada XDJ-RX3 dilakukan melalui shim dinamis (`fbshim.so`) yang di-inject via `RB_LD_PRELOAD` langsung ke dalam address space `rbp`.

---

## 2. MOD 1: Touch Preview / Song Info Remember

### A. Masalah
Secara default pada firmware Pioneer, panel Touch Preview dan Song Info di sebelah kanan list track akan kembali ke kondisi default (off/tertutup) setiap kali player di-booting atau ganti media, sehingga DJ harus mengetuk ikon preview berulang kali setiap memulai sesi.

### B. Analisis Teknis di `rbp`
- **Fungsi Pemeriksaan**: `UiBrowseComm_IsSongInfoDispON` (`0x00106584`) memeriksa apakah Song Info aktif.
- **Fungsi Update Status**: `UiBrowseComm_UpdateSongInfoFlg` (`0x001062b0`) dan `UiBrowseComm_SetRightInfoOnFlg` (`0x00105da4`).
- **Touch Preview Engine**:
  - `UiBrowsePreviewLoad_Start` (`0x001219dc`)
  - `UiBrowsePreviewLoad_Stop` (`0x00121a00`)
  - `BrowseUiIf::TouchPreviewProc` (`0x000d0014`)
- **State Flag di Memori**:
  - Lokasi memori internal di `0x03254d00` dan `s_PreviewLoadStart` di `0x0216c360`.

### C. Desain Solusi
1. **Hooking Point**: Intercept pada `UiBrowseComm_IsSongInfoDispON` atau saat inisialisasi browse `UiBrowseComm_InitUiBrowse` (`0x00104ed8`).
2. **Persistence Storage**: Menyimpan preferensi ke file `/opt/rblive4/settings/browse_preview.state`.
3. **Logic**:
   - Jika file `browse_preview.state` berisi `1`, saat pertama kali browse list dimuat, otomatis aktifkan flag Song Info/Touch Preview tanpa memerlukan tap manual.
   - Setiap kali event tap Touch Preview atau tombol Info diubah oleh user, perbarui file state.

---

## 3. MOD 2: Custom Columns & Row Formatting

### A. Masalah
Tampilan bawaan baris track pada XDJ-RX3 memiliki lebar dan susunan kolom yang kaku. Pada beberapa genre atau situasi panggung, DJ membutuhkan:
- Menyembunyikan kolom Track Number (`#`) agar judul lagu yang panjang tidak terpotong.
- Menampilkan kombinasi BPM dan Key secara bersamaan dengan layout yang rapi.
- Menambahkan informasi sekunder (seperti Release Year, Genre, atau Rating) ke dalam baris track.

### B. Analisis Teknis di `rbp`
- **Tabel Objek Baris (12 Baris per Layar)**:
  - `ID_LINE_TEXT` (`0x00526af0`): Array 12 ObjectID teks baris `[103, 100, 97, 94, 91, 88, 85, 82, 79, 76, 73, 70]`.
  - `ID_LIST_LEFT_NUM` (`0x004c9990`): Array 12 ObjectID nomor track `[262, 254, 246, 238, 230, ...]`.
  - `previewBGRectId` (`0x004c9948`): Array 12 ObjectID Touch Preview mini-wave `[267, 259, 251, 243, ...]`.
  - `ID_LIST_LEFT_BG` (`0x004c9960`): Background baris `[265, 257, 249, 241, ...]`.
- **Fungsi Render**:
  - `ui_ListBrowse_SetText` (`0x00299b9c`): Mengatur teks judul, artis, BPM, dan Key.
  - `ui_com_draw_SetObjectPosLeft` (`0x0018e2cc`): Mengatur margin kiri teks.
  - `ui_com_draw_SetLinkState` (`0x0018dca0`): Menyembunyikan / menampilkan elemen objek (LinkState 0 = hidden, 1 = visible).
  - `ui_com_draw_SetTextStringByBuffer` (`0x0018df24`): Menulis string langsung ke buffer tampilan.

### C. Desain Solusi
1. **Konfigurasi Kolom**:
   - Environment variables / config file `/opt/rblive4/settings/browse_columns.conf`:
     - `BROWSE_HIDE_TRACKNO=1`: Menyembunyikan `ID_LIST_LEFT_NUM` dan menggeser teks ke kiri menggunakan `ui_com_draw_SetObjectPosLeft`.
     - `BROWSE_COMPACT_ART=1`: Menyesuaikan proporsi artwork.
     - `BROWSE_ENRICH_TEXT=1`: Menggabungkan metadata ekstra (BPM + Key + Year) ke buffer baris.
2. **Hooking Point**:
   - Intercept fungsi `ui_ListBrowse_SetText` atau wrap `ui_com_draw_SetTextStringByBuffer`.
   - Mengatur link state objek via `ui_com_draw_SetLinkState` untuk visibilitas nomor urut dan preview.

---

## 4. MOD 3: Camelot Key / Mixed-In-Key Display

### A. Masalah
Pioneer secara default sering kali menampilkan format Musical Key klasik (`Am`, `C`, `F#m`) tergantung pengaturan rekordbox USB. Kebanyakan DJ modern lebih memilih format numerik Camelot (`8A`, `8B`, `11B`) untuk harmonic mixing cepat.

### B. Analisis Teknis di `rbp`
- **Fungsi Utama**:
  1. `UiBrowseComm_IsKeyDispStyleMixedInKey` (`0x0010cf88`):
     - Memeriksa style setting rekordbox browse. Return `1` untuk Camelot, `0` untuk Classic.
  2. `CmnFunc_CmnInfo_GetDevSetKeyCategoryDispStyle` (`0x00185a8c`):
     - Dipanggil oleh **seluruh komponen rbp** (11 pemanggil: `GetAllRecFromDB`, `ReplaceKeyString`, `GetRightPrepareList`, `GetRightBrowseOneData`, `Dsql_getNewKeyName`, `ExecInformationJump`, dll).
     - Mengembalikan nilai `2` untuk Alphanumeric / Camelot, atau `1` untuk Classic.
  3. `DBCommon_CopyKeyNameStr` (`0x001411d0`):
     - Rutin translasi string key dari database/metadata ke format tampilan sesuai parameter `style` (`r2 = 1` untuk Camelot).
  4. `DBCommon_GetKeyName` (`0x001410f4`):
     - Pencarian nama key langsung berdasarkan Key ID integer (`1`..`24`). Parameter `style = 1` memilih Camelot string.
  5. `Pioneer Universal 24-Key Table` (`0x00515390`):
     - Tabel global berisi 24 pasang pointer UTF-16LE `(classic_ptr, camelot_ptr)` dari `1A`/`Abm` hingga `12B`/`E`.

### C. Desain Solusi (5-Tier Universal Override)
1. **Tier 1 (Browse Query)**: Hook `UiBrowseComm_IsKeyDispStyleMixedInKey` (`0x0010cf88`) return `1`.
2. **Tier 2 (Global Device Setting)**: Hook `CmnFunc_CmnInfo_GetDevSetKeyCategoryDispStyle` (`0x00185a8c`) return `2` (Camelot mode untuk 11 modul pemanggil internal).
3. **Tier 3 (String Translator)**: Hook `DBCommon_CopyKeyNameStr` (`0x001411d0`) memaksakan parameter `style = 1` pada saat pemanggilan.
4. **Tier 4 (Direct Key Lookup)**: Hook `DBCommon_GetKeyName` (`0x001410f4`) memaksakan parameter `style = 1`.
5. **Tier 5 (Memory Table Patch)**: Menimpa slot pointer `classic_ptr` pada tabel `0x00515390` di memory agar langsung menunjuk ke pointer `camelot_ptr`, menjamin keamanan 100% jika ada modul yang membaca tabel secara langsung.

---

## 5. Implementasi & Roadmap

### Fase 1: Dokumentasi & Desain Arsitektur (Selesai)
- Meneliti binary `rbp`, memetakan VMA simbol, dan menuliskan rencana teknis di dokumen ini.

### Fase 2: Implementasi Modul C di `scripts/shims/`
1. Membuat modul baru `scripts/shims/browse_shim.c` (dan header `browse_shim.h`).
2. Mendaftarkan `browse_shim.o` ke dalam `scripts/shims/Makefile` sebagai bagian dari `fbshim.so`.
3. Menerapkan hook memori:
   - State persistence untuk Touch Preview / Song Info.
   - Kustomisasi kolom baris (`BROWSE_HIDE_TRACKNO`, formatting string).
   - Override Camelot Key display.
4. Cross-compile di target board S905X dan deploy ke `/opt/rblive4/fbshim.so` dan chroot `/opt/rblive4/rbx3-run/usr/lib/fbshim.so`.
5. Verifikasi fungsionalitas langsung di layar VNC (`http://unx.local:5800/`).

