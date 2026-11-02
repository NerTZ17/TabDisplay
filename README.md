# TabDisplay

Turn an Android tablet (or phone) into a real extra monitor for a Windows PC, over a USB cable, at the panel's native resolution and refresh rate, with touch and stylus input.

Built with [Claude](https://claude.ai) by **NerTZ – SONO Solutions** · [sono-solutions.com](https://sono-solutions.com)

## The story

I bought a Samsung tablet for work at the office. After a long stretch of working from home it mostly sat in a drawer, and one day I thought: why not make it a third monitor for my PC?

It turned out to be harder than it sounded:

1. **No Wi-Fi on my PC.** My desktop has no wireless adapter, so Samsung's built-in Second Screen (which is wireless only) could not even start.
2. **Wireless adapters were not good enough.** I tried with an adapter anyway; the result lagged and stuttered too much for real work.
3. **No wired option out of the box.** Samsung tablets have no built-in way to act as a wired display, and Windows cannot use a tablet as a monitor over USB on its own.
4. **So I researched and built it with Claude.** I worked through the problem with Claude to connect the tablet over a cable as a proper extra monitor, pushing for the full native resolution and refresh rate.

The result is TabDisplay. It runs well on my own setup every day: a Galaxy Tab S10 Ultra as a 2960×1848 @ 120 Hz third monitor, with touch and S Pen. If you need something added or changed, open an issue or reach out through [sono-solutions.com](https://sono-solutions.com) and I will try to build it properly.

## Features

- Extends the Windows desktop to the tablet as a real monitor (not a mirror), at the tablet's native resolution and up to its highest refresh rate.
- Low latency: hardware H.265/HEVC or H.264 encoding on the PC, hardware decoding on the tablet, frames acknowledged one by one so lag never piles up.
- Touch (10 fingers) and stylus with pressure, tilt, hover and side button, injected as real Windows touch and pen input; palm rejection while the pen is near.
- Touch never steals your mouse. Tap, scroll or draw on the tablet, and the mouse cursor jumps back to where it was on your other monitors, so you can keep working there without looking for it.
- Tray app with an On/Off switch. Plug the tablet in and the extra monitor appears and the tablet app opens by itself; unplug it and Windows goes back to your normal monitors.
- Starts with Windows, remembers its state, recovers on its own from cable drops and app restarts.

Measured on the author's PC (RTX 3070, Galaxy Tab S10 Ultra, USB 3): 120 fps at 2960×1848, about 21 ms from the PC to the tablet's screen and back.

## Requirements

| | |
|---|---|
| Windows | Windows 10 (version 1903 or newer) or Windows 11, 64-bit |
| PC graphics | A GPU with a hardware video encoder: NVIDIA (NVENC), or AMD / Intel through Media Foundation |
| Virtual monitor | [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (free, signed) |
| adb | [Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools) |
| Android | Android 8.0 or newer, tablet or phone, with USB debugging enabled |
| Cable | A USB **data** cable, plugged directly into the PC (USB 3 recommended) |

## Quick start

1. Install Virtual Display Driver and add your tablet's resolution and refresh rate to it.
2. Build or download `TabDisplay.exe` and put the Platform-Tools files (`adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`) next to it.
3. Install the TabDisplay APK on the tablet and enable USB debugging.
4. Run `TabDisplay.exe`, plug the tablet in, and accept the USB debugging prompt.

The full walkthrough, including how to find your tablet's resolution, is in **[docs/SETUP.md](docs/SETUP.md)**.

## Security warnings on first run

TabDisplay is a free, open-source hobby project, so its files are **not digitally signed**. Signing a Windows program needs a paid code-signing certificate, and publishing on Google Play needs a developer account; neither is in place yet. Because of that, you will see these warnings the first time:

- **Windows SmartScreen** ("Windows protected your PC"): click **More info → Run anyway**. TabDisplay only talks to the tablet through adb on `127.0.0.1` over the USB cable; it does not open network ports or connect to the internet.
- **Android** ("Install unknown apps" / Play Protect): allow installing from the source you used (adb or your file manager) and confirm the install.

If you prefer not to trust a downloaded binary, build both apps yourself from this source code (see [docs/SETUP.md](docs/SETUP.md)) so you know exactly what you run. Signed releases may come later if the project grows.

## How it works

```
Virtual Display Driver       a monitor Windows can extend the desktop to
  -> DXGI Desktop Duplication  GPU texture of that monitor, no CPU copies
  -> NVENC / Media Foundation  hardware HEVC or H.264, low-latency CBR
  -> adb forward over USB      one TCP stream through the cable
  -> Android MediaCodec        hardware decoder, rendered straight to the screen
  <- acknowledgements, touch and pen
  -> InjectSyntheticPointerInput  real Windows touch and pen events
```

The wire protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Project layout

| Folder | Contents |
|---|---|
| `host/` | Windows app (C++20, CMake). `TabDisplay.exe` is the tray app, `tabdisplay-host.exe` the console version for debugging and benchmarks |
| `android/` | Android app (Kotlin, package `com.nertz.tabdisplay`) |
| `docs/` | Setup guide and protocol |

## Limitations

- **Netflix and other DRM video show black.** Protected video is blanked by Windows in every screen-capture path, and a virtual monitor has no HDCP. This is by design and TabDisplay does not work around it; use the streaming app on the tablet itself.
- USB debugging must stay enabled on the tablet.
- No audio forwarding yet.
- Tested on one PC and one tablet so far. Reports from other hardware are welcome.

## Credits

- Built with [Claude](https://claude.ai) by Anthropic, as a research and coding partner.
- [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) for the virtual monitor.
- NVIDIA Video Codec SDK header via [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT © 2026 NerTZ – SONO Solutions. See [LICENSE](LICENSE).

TabDisplay is an independent project and is not affiliated with Samsung, Microsoft, NVIDIA, Google or Anthropic.
