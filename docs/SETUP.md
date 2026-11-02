# TabDisplay setup guide

This guide takes you from nothing to a working wired extra monitor. It takes about 20 minutes the first time.

## 1. What you need

- A Windows 10 (1903+) or Windows 11 64-bit PC with an NVIDIA, AMD or Intel GPU.
- An Android 8.0+ tablet or phone.
- A USB **data** cable. Many charging cables only carry power or USB 2.0 data; a USB 3 C-to-C or A-to-C data cable gives the best results.

## 2. Prepare the tablet

1. Open **Settings → About tablet → Software information** and tap **Build number** seven times to unlock Developer options. (On non-Samsung devices it is usually **Settings → About phone → Build number**.)
2. Open **Settings → Developer options** and turn on **USB debugging**.
3. Find the tablet's screen resolution and refresh rate. Two easy ways:
   - Run the TabDisplay app once (step 4); with the PC host running, its log prints a line like `tablet: 2960x1848, max 120 Hz`.
   - Or look up your model's display specs. Use the landscape orientation, larger number first.
4. On Samsung devices, set **Settings → Display → Motion smoothness** to **Adaptive** (or High) so 120 Hz modes are available.

## 3. Prepare the PC

### 3.1 Platform-Tools (adb)

1. Download [Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools) for Windows and unzip it.
2. Copy `adb.exe`, `AdbWinApi.dll` and `AdbWinUsbApi.dll` into the same folder as `TabDisplay.exe` (or keep the whole `platform-tools` folder next to it). TabDisplay also finds adb on `PATH` or in `%LOCALAPPDATA%\Android\Sdk\platform-tools`.

### 3.2 Virtual Display Driver

TabDisplay needs a monitor for Windows to extend the desktop to. Virtual Display Driver provides one.

1. Download the latest release of [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases) (the **Virtual Driver Control** package) and install the display driver from it. Windows asks for administrator approval.
2. Open `C:\VirtualDisplayDriver\vdd_settings.xml` and make the resolution list match your tablet, for example for a 2960×1848 @ 120 Hz panel:

   ```xml
   <vdd_settings>
       <monitors>
           <count>1</count>
       </monitors>
       <gpu>
           <friendlyname>default</friendlyname>
       </gpu>
       <global>
           <g_refresh_rate>60</g_refresh_rate>
           <g_refresh_rate>120</g_refresh_rate>
       </global>
       <resolutions>
           <resolution>
               <width>2960</width>
               <height>1848</height>
               <refresh_rate>120</refresh_rate>
           </resolution>
       </resolutions>
   </vdd_settings>
   ```

3. Reload the driver (Virtual Driver Control → Reload, or disable and enable "Virtual Display Driver" in Device Manager).
4. In **Settings → System → Display**, select the new monitor and:
   - set **Extend these displays**,
   - pick your tablet's resolution and a scale you like,
   - under **Advanced display**, pick the highest refresh rate,
   - drag it to where the tablet physically sits next to your other monitors.

TabDisplay remembers this layout: it detaches the virtual monitor when the tablet is unplugged and restores it, with the same position and mode, when the tablet comes back.

### 3.3 Build TabDisplay

Requirements: Visual Studio 2022 or newer (Build Tools are enough) with the **Desktop development with C++** workload, and the Windows 10/11 SDK.

```bash
host\build.cmd
```

The output is in `host\build\`: `TabDisplay.exe` (tray app) and `tabdisplay-host.exe` (console).

### 3.4 Build and install the Android app

Requirements: Android Studio (or the Android SDK with JDK 17+).

```bash
cd android && gradlew.bat assembleDebug
```

Install it with the tablet connected:

```bash
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

## 4. Daily use

1. Run `TabDisplay.exe` once. It adds itself to Windows startup and lives in the system tray (click the **^** on the taskbar if the icon is hidden; drag it out to keep it visible).
2. Plug the tablet in and accept **Allow USB debugging** on the tablet (tick **Always allow**).
3. TabDisplay attaches the virtual monitor, opens the app on the tablet and starts streaming.

| Situation | What happens |
|---|---|
| Switch **On**, tablet plugged in | Virtual monitor attached, tablet app opened (once per plug-in), streaming |
| Tablet unplugged | Virtual monitor detached, Windows returns to your other monitors |
| Switch **Off** | Streaming stops, virtual monitor detached |
| Click the tray icon | Open or close the panel |
| Right-click the tray icon → Exit | Stop everything and quit |

Tray icon dot: green connected, yellow waiting, red needs attention, no dot off. The info button in the panel shows latency, frame rate, bitrate, codec and encoder.

Settings live in `HKCU\Software\NerTZ\TabDisplay`; the startup entry is `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\TabDisplay`. The log is `%LOCALAPPDATA%\TabDisplay\tabdisplay.log`.

Only one of `TabDisplay.exe` or `tabdisplay-host.exe` can run at a time.

## 5. Console host (debugging)

```bash
host\build\tabdisplay-host.exe --log-input
```

| Option | Meaning |
|---|---|
| `--list` | List monitors |
| `--monitor <name>` | `\\.\DISPLAYn` or part of the monitor name (default: the first VDD/virtual monitor) |
| `--codec auto\|hevc\|h264` | Codec (default `auto`: HEVC when the tablet decodes it at the target rate, else H.264) |
| `--encoder auto\|nvenc\|mf` | Encoder (default `auto`: NVENC on NVIDIA, Media Foundation otherwise or as fallback) |
| `--fps <n>` | Target frame rate (default: monitor refresh rate) |
| `--bitrate <mbps>` | CBR bitrate (default 80) |
| `--inflight <n>` | Frames allowed without acknowledgement (default 3) |
| `--force` | Ignore decoder limits reported by the tablet |
| `--serial <id>` | adb serial when several devices are connected |
| `--no-adb` | Do not use adb (forward port 27183 yourself) |
| `--keep-monitor` | Do not attach/detach the virtual monitor |
| `--no-launch` | Do not open the tablet app automatically |
| `--monitor-on` / `--monitor-off` | Attach / detach the virtual monitor and exit |
| `--log-input` | Print touch and pen events |
| `--no-cursor-restore` | Leave the mouse cursor where a tablet tap moved it |
| `--bench <seconds>` | Capture and encode locally, no tablet needed |
| `--stress` | With `--bench`: encode at the target rate even when the screen is static |
| `--dump <file>` | With `--bench`: write the encoded stream to a file |

The console prints one line per second: captured and encoded fps, encode time, Mbps, skipped frames, frames in flight, round-trip time and tablet decode time.

## 6. Tuning

- **Sharper text:** raise `--bitrate` (100–150).
- **Lower or steadier latency on NVIDIA:** NVIDIA Control Panel → Manage 3D settings → Program Settings → add `TabDisplay.exe` → Power management mode **Prefer maximum performance**. Otherwise the GPU clocks down when the screen is quiet and encoding gets slower.
- **Tablet shows fewer fps than expected:** check the virtual monitor's refresh rate in Windows, Motion smoothness on the tablet, and whether the log says `decoder reports max N fps ... capping` (try `--force`).
- **Resolution mismatch:** if the virtual monitor and the tablet differ, the picture is letterboxed. Set the virtual monitor to the tablet's exact resolution for 1:1 pixels.

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Panel says "adb not responding" or adb shows `offline` | adb on the tablet got stuck after a USB reset | Toggle USB debugging off and on, or unplug and replug |
| Panel says "Allow USB debugging on the tablet" | The PC is not authorised yet | Accept the prompt on the tablet, tick **Always allow** |
| Repeated disconnects | USB hub, front-panel port or a charging-only cable | Plug directly into a rear motherboard port with a USB 3 data cable |
| "Virtual monitor not found" | Virtual Display Driver missing or disabled | Install/enable it (section 3.2) |
| "No hardware video encoder available" | GPU driver missing or GPU without a hardware encoder | Update the GPU driver |
| Netflix / DRM video is black | Protected content is blanked in screen capture by design | Watch it in the app on the tablet |
| Touch lands in the wrong place | Display layout changed while streaming | It re-maps automatically within a quarter second; if not, toggle Off/On |
