# TabDisplay protocol v1

Transport: one TCP connection. The Android app listens on `127.0.0.1:27183` (loopback only). The PC host runs `adb forward tcp:27183 tcp:27183` and connects to `127.0.0.1:27183` on the PC. The host retries every second until the handshake succeeds, because `adb forward` accepts the PC-side connection even when the app is not listening yet and then closes it.

All integers are little-endian. Coordinates sent by the tablet are normalized to `0..65535` over the decoded video area, not the view.

## Framing

Every message:

| Field | Type | Notes |
|---|---|---|
| length | u32 | Byte count of `type` + `payload` |
| type | u8 | See table below |
| payload | bytes | `length - 1` bytes |

The receiver rejects `length == 0` or `length > 16 MiB`.

## Session flow

1. Host connects and sends `Hello`.
2. Tablet answers `ClientHello` with its display and decoder capabilities.
3. Host chooses codec, size and frame rate, then sends `StreamConfig`.
4. Host sends one `VideoFrame` with flag `CODEC_CONFIG` (parameter sets only), then frames. The first picture is always an IDR.
5. Tablet sends `FrameAck` after it releases each decoded picture to the display.
6. Tablet sends `Touch`, `Pen` and `RequestKeyframe` at any time after `StreamConfig`.
7. Host sends `CursorShape` / `CursorPos` at any time after `StreamConfig`.

Either side closes the socket to end the session. The tablet then goes back to listening.

## Messages

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | Hello | host → tablet | `magic[4] = "TDSP"`, `u16 version = 1` |
| 2 | ClientHello | tablet → host | see below |
| 3 | StreamConfig | host → tablet | `u8 codec`, `u16 width`, `u16 height`, `u16 fps`, `u32 bitrateKbps` |
| 4 | VideoFrame | host → tablet | `u32 frameId`, `u64 hostTimeUs`, `u8 flags`, then Annex-B bytes to the end of the message |
| 5 | FrameAck | tablet → host | `u32 frameId`, `u64 hostTimeUs` (echo of the frame), `u32 decodeUs` |
| 6 | Touch | tablet → host | `u8 count`, then `count` × (`u8 id`, `u8 flags`, `u16 x`, `u16 y`) |
| 7 | Pen | tablet → host | `u8 flags`, `u16 x`, `u16 y`, `u16 pressure`, `i8 tiltX`, `i8 tiltY`, `u16 rotation` |
| 8 | RequestKeyframe | tablet → host | empty |
| 9 | CursorShape | host → tablet | `u16 width`, `u16 height`, `u16 hotX`, `u16 hotY`, then `width*height*4` bytes BGRA, straight alpha, top-down rows |
| 10 | CursorPos | host → tablet | `i16 x`, `i16 y` (pointer hot spot position in stream pixels), `u8 visible` |

| 11 | Heartbeat | both | empty |

The tablet draws the cursor bitmap with its top-left corner at `(x − hotX, y − hotY)`, scaled by the same factor as the video.

## Liveness

Each side sends `Heartbeat` when it has sent nothing else for 1 second. Each side ends the session when it has received nothing for 4 seconds, so a connection that dies inside `adb` without a FIN is detected. The host also uses a 3 second socket send timeout.

The tablet keeps listening while a session runs. When a new host connects, the tablet closes the current session and serves the new one (newest host wins). Only one host process runs per Windows session (named mutex `Local\TabDisplayHost`).

### ClientHello payload

| Field | Type | Notes |
|---|---|---|
| magic | 4 bytes | `"TDSP"` |
| version | u16 | `1` |
| width | u16 | Native panel width in landscape (larger side) |
| height | u16 | Native panel height in landscape (smaller side) |
| maxRefreshHz | u16 | Highest refresh rate among display modes at native size, rounded |
| codecCount | u8 | Number of codec entries |
| codec entries | repeated | `u8 codec`, `u8 flags`, `u16 maxFps`, `u16 maxWidth`, `u16 maxHeight` |

Codec entry flags: bit 0 hardware decoder, bit 1 `FEATURE_LowLatency` supported.
`maxFps` is the upper bound of `VideoCapabilities.getSupportedFrameRatesFor(width, height)` for the native size, floored; `0` when the native size is not supported.

### Codec ids

| Id | Codec | Android MIME |
|---|---|---|
| 1 | HEVC | `video/hevc` |
| 2 | H.264 | `video/avc` |

### VideoFrame flags

| Bit | Meaning |
|---|---|
| 0 | IDR picture |
| 1 | `CODEC_CONFIG`: payload holds only parameter sets (VPS/SPS/PPS); queue with `BUFFER_FLAG_CODEC_CONFIG` and do not ack |

### Touch

One message carries every finger that is currently down plus fingers that lifted in this event.

| Pointer flag bit | Meaning |
|---|---|
| 0 | In contact (0 means this finger lifted) |
| 1 | Canceled (gesture canceled, host lifts without click) |

`id` is the Android pointer id, `0..9`. Ids above 9 are dropped by the tablet.

### Pen

| Flag bit | Meaning |
|---|---|
| 0 | In range (hovering or touching). 0 means the pen left range |
| 1 | In contact |
| 2 | Barrel (side) button pressed |
| 3 | Eraser end / eraser tool |

`pressure` is `0..1024`. `tiltX` and `tiltY` are degrees `-90..90` in Windows pen convention. `rotation` is degrees `0..359`.
The tablet sends one `Pen` message per historical sample of a `MotionEvent`, oldest first.

## Flow control

The host keeps at most 3 frames without `FrameAck` (configurable). When the limit is reached it skips capture frames instead of queueing them, and encodes the newest captured picture once an ack arrives. Round-trip time = host clock now − echoed `hostTimeUs`.

If the tablet drops a frame (no decoder input buffer, decode error), it sends `RequestKeyframe` and discards frames until the next IDR.
