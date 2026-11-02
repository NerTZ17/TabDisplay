# Setup: Tab S10 Ultra sebagai monitor ke-3 lewat kabel

## 1. Monitor virtual di Windows (Virtual Display Driver)

1. Download rilis terbaru **Virtual Display Driver** dari GitHub `VirtualDrivers/Virtual-Display-Driver` (installer atau paket "Virtual Driver Control").
2. Install sesuai petunjuk di halaman rilis. Driver ini sudah ditandatangani, jadi tidak perlu test-signing.
3. Tambahkan mode **2960×1848 @ 120 Hz** di daftar resolusi VDD:
   - lewat aplikasi Virtual Driver Control, atau
   - edit `vdd_settings.xml` (lokasi ditunjukkan aplikasi kontrolnya), tambah entri resolusi `2960 × 1848` dengan refresh `120`, lalu reload/restart driver.
4. Settings → System → Display:
   - pilih monitor virtual, set **Extend these displays**,
   - resolusi **2960 × 1848**, scale sesuai selera (misalnya 200%),
   - Advanced display → refresh rate **120 Hz**,
   - atur posisinya terhadap 2 monitor fisik.
5. Cek dari PC:

   ```bash
   host\build\tabdisplay-host.exe --list
   ```

   Monitor VDD harus muncul dengan `2960x1848 @ 120 Hz` dan `on NVIDIA GeForce RTX 3070`.

Kalau namanya tidak mengandung "VDD" atau "Virtual", pilih manual dengan `--monitor \\.\DISPLAYn`.

Instalasi di PC ini (2 November 2026): Virtual Display Driver 25.7.23 (paket `VDD.Control.25.7.23.zip`, SHA-256 `a701f227…9154`) dipasang dengan `devcon install MttVDD.inf Root\MttVDD`. Konfigurasi ada di `C:\VirtualDisplayDriver\vdd_settings.xml` (mode 2960×1848, 1920×1200, 1920×1080; refresh 60 dan 120). Monitor muncul sebagai `\\.\DISPLAY6` "VDD by MTT", 2960×1848 @ 120 Hz, di kanan monitor MSI.

Lepas driver: app VDD Control (Uninstall), atau Device Manager → Display adapters → Virtual Display Driver → Uninstall device.

## 2. Tablet

1. Settings → About tablet → Software information → ketuk **Build number** 7 kali.
2. Developer options → aktifkan **USB debugging**.
3. Sambungkan kabel USB-C ke port **USB 3** di PC (port biru / USB-C di motherboard, hindari hub).
4. Izinkan dialog "Allow USB debugging" (centang "Always allow").
5. Cek:

   ```bash
   adb devices
   ```

## 3. Build

Host Windows:

```bash
host\build.cmd
```

APK Android:

```bash
cd android && gradlew.bat assembleDebug
```

Install ke tablet:

```bash
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

## 4. Jalankan

1. Buka app **TabDisplay** di tablet. Layar menunjukkan status menunggu PC.
2. Di PC:

   ```bash
   host\build\tabdisplay-host.exe
   ```

   Host menjalankan `adb forward tcp:27183 tcp:27183`, menghubungi app, memilih codec dan fps dari kemampuan decoder tablet, lalu mulai streaming.
3. Tombol **volume turun** di tablet: tampilkan/sembunyikan statistik.
4. `Ctrl+C` di PC untuk berhenti.

## Opsi host

| Opsi | Fungsi |
|---|---|
| `--list` | Daftar monitor |
| `--monitor <nama>` | `\\.\DISPLAYn` atau bagian nama monitor |
| `--codec auto\|hevc\|h264` | Pilih codec (default `auto`) |
| `--fps <n>` | Target fps (default refresh monitor) |
| `--bitrate <mbps>` | Bitrate CBR (default 80) |
| `--inflight <n>` | Frame tanpa ack yang diizinkan (default 3; 2 membatasi ke ±100 fps karena RTT ±20 ms) |
| `--force` | Abaikan batas decoder yang dilaporkan tablet |
| `--serial <id>` | Serial adb kalau ada beberapa perangkat |
| `--no-adb` | Jangan jalankan `adb forward` |
| `--log-input` | Tampilkan event sentuhan dan pena dari tablet |
| `--bench <detik>` | Uji capture + encode tanpa tablet |
| `--stress` | Dengan `--bench`: encode terus walau layar diam |
| `--dump <file>` | Dengan `--bench`: simpan stream ke file |

## Tuning resolusi dan refresh maksimum

- Log host per detik: `cap` (frame dari Windows), `enc` (frame terkirim), `ms` encode, Mbps, `skip` (frame dilewati karena flow control), `rtt` (PC → tablet tampil → ack), `decode`.
- Kalau host menulis `decoder reports max N fps ... capping`, decoder tablet melaporkan batas di bawah 120. Batas ini sering konservatif: coba `--force` dan lihat `ack fps` di log.
- Kalau `skip` tinggi dan `rtt` naik, decoder atau USB tidak mengejar: turunkan `--bitrate`, atau coba `--codec h264`.
- `cap` di bawah 120 saat ada gerakan berarti Windows tidak menyajikan monitor virtual di 120 Hz: cek refresh rate di Advanced display.
- Teks lebih tajam: naikkan `--bitrate` (misalnya 120–150).
- Encode melambat saat layar jarang berubah (GPU turun clock; terukur 2 ms → 6,5 ms di 24 fps). Untuk latency stabil: NVIDIA Control Panel → Manage 3D settings → Program Settings → tambah `tabdisplay-host.exe` → Power management mode **Prefer maximum performance**.

## Masalah umum

| Gejala | Penyebab | Solusi |
|---|---|---|
| `no matching monitor` | VDD belum aktif / nama beda | `--list`, lalu `--monitor` |
| `NVENC needs it on the NVIDIA GPU` | Monitor virtual dirender adapter lain | Set GPU render VDD ke RTX 3070 di pengaturan VDD |
| `adb forward failed` | Tablet belum diizinkan / kabel | `adb devices`, izinkan dialog di tablet |
| `waiting for the tablet app` terus | App belum dibuka | Buka TabDisplay di tablet |
| `driver supports NVENC API ...` | Driver NVIDIA terlalu lama | Update driver NVIDIA |
