# TabDisplay

Prototipe monitor tambahan lewat kabel USB: PC Windows (RTX 3070) menjadikan Samsung Galaxy Tab S10 Ultra monitor ke-3 di resolusi native 2960×1848 dan 120 Hz, dengan sentuhan dan S Pen.

## Cara kerja

```
Virtual Display Driver (IddCx, monitor ke-3)
  -> DXGI Desktop Duplication (texture BGRA di GPU)
  -> NVENC HEVC/H.264, preset P1, ultra-low-latency, CBR
  -> TCP lewat adb forward (USB)
  -> MediaCodec low-latency -> SurfaceView 120 Hz (tablet)
  <- FrameAck, sentuhan, S Pen (tablet)
  -> InjectSyntheticPointerInput (PC)
```

- Codec dan fps dipilih otomatis dari kemampuan decoder tablet (HEVC@120 → HEVC@90 → HEVC@60 → H.264@60).
- Maksimal 3 frame belum di-ack. Frame yang tidak sempat dikirim dilewati, bukan diantrekan, supaya latency tidak menumpuk.
- Kursor tidak di-encode: bentuk dan posisinya dikirim terpisah dan digambar di tablet.

## Struktur

| Folder | Isi |
|---|---|
| `host/` | Host Windows C++20 (CMake + Ninja, MSVC) |
| `android/` | App Android Kotlin (`com.nertz.tabdisplay`) |
| `docs/PROTOCOL.md` | Spesifikasi protokol v1 |
| `docs/SETUP.md` | Install driver, build, jalankan, tuning |

## Mulai cepat

Lihat [docs/SETUP.md](docs/SETUP.md).

## Hasil uji yang sudah ada

Encoder di RTX 3070, mode `--bench --stress` pada monitor 2560×1080 (belum di 2960×1848 karena VDD belum terpasang):

| Codec | Encode rata-rata | Throughput |
|---|---|---|
| HEVC | 2,3 ms | ±260 fps |
| H.264 | 2,9 ms | ±127 fps (dibatasi target 120) |

2960×1848 punya ±2× piksel, jadi perkiraan encode ±4,5–6 ms per frame, masih di bawah anggaran 8,3 ms untuk 120 fps.

End-to-end ke Tab S10 Ultra (SM-X920, Android 16, MT6989) lewat `adb forward`, sumber monitor 1920×1080 (VDD belum terpasang):

| Setting | fps diterima tablet | RTT | Decode tablet |
|---|---|---|---|
| HEVC, inflight 2 | 88–108 | 16–21 ms | 10–13 ms |
| HEVC, inflight 3, pacing tetap | 120 | 20–21 ms | 11,5 ms |
| H.264, inflight 3 | 117–118 | 18–19 ms | 11 ms |

Native, monitor VDD 2960×1848 @ 120 Hz (Virtual Display Driver 25.7.23), sumber animasi canvas 120 fps:

| Codec | fps diterima tablet | RTT | Decode tablet | Encode PC | Bitrate |
|---|---|---|---|---|---|
| HEVC, inflight 3 | 119–120 | 21–21,6 ms | 12–12,4 ms | 4,2–4,5 ms | ±31 Mbps |

NVENC di 2960×1848 (`--bench --stress`): ±273 fps, rata-rata 3,0 ms saat GPU di clock penuh.

Decoder tablet: `c2.mtk.hevc.decoder` melaporkan maks 389 fps di 2960×1848, `c2.mtk.avc.decoder` 193 fps. Panel punya mode 30/60/120 Hz. Bagian terbesar latency adalah decode di tablet (±11 ms).

## Belum termasuk

- AOAP (USB tanpa USB debugging)
- Audio
- Auto-start dan UI tray di PC
- Pipelining encode async (perlu kalau encode 3K mendekati 8 ms)
