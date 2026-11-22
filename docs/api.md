# Custom firmware: HTTP API

The firmware joins the Wi-Fi network saved on the device (it reuses the one the stock firmware
stored) and is reachable as **`http://holoneo.local/`** (mDNS) on port 80.

If no network is saved or it can't connect for ~10 attempts, it opens a WPA2 access point
**`Holoneo-setup`** whose password is the API token (shown on the screen). Then use
`POST /wifi` at `http://192.168.4.1/`.

## Token

Every endpoint except `GET /status` needs the device's API token, generated on first boot and
stored on the device. **Hold your hand in front of the sensor** to show it on the screen
(it is also printed on the serial console at boot).

Send it as `Authorization: Bearer <token>` or as `?token=<token>`.

```sh
export HOLO_TOKEN=...            # from the screen
H=http://holoneo.local
A="Authorization: Bearer $HOLO_TOKEN"
```

## Endpoints

| Method & path | Body / query | Does |
|---|---|---|
| `GET /status` | – | JSON: version, IP, RSSI, uptime, free memory, volume, gesture count (no token needed) |
| `POST /text` | text body; `?size=1..4&color=RRGGBB&bg=RRGGBB` | clears the screen and shows word-wrapped, centred text |
| `POST /image` | baseline JPEG (≤ 2 MB) | scales down by 1/2, 1/4, 1/8 until it fits 320×240, centres on black |
| `POST /fill` | `?color=RRGGBB` | fills the screen |
| `POST /backlight` | `?on=0\|1` | backlight off/on |
| `POST /audio` | WAV, 16-bit PCM, mono/stereo, any common rate (≤ 4 MB) | plays it (the reply comes before playback starts) |
| `POST /volume` | `?level=0..100` | speaker volume |
| `GET /mic` | `?seconds=1..30` | records the microphones, returns a 16 kHz mono WAV |
| `POST /play` | `?path=/data/...\|/sd/...&loop=0\|1&fps=N`, or upload a clip as body (≤ 6 MB) | plays an MJPEG AVI or concatenated-JPEG clip (`.mjpeg`); up to 30 fps at 320×240 |
| `POST /stop` | – | stops playback |
| `GET /files/<path>` | e.g. `/files/data`, `/files/sd/Video` | lists a directory (JSON) or downloads a file |
| `PUT /files/<path>` | file body | stores a file under `/data` (internal flash, ~10 MB) or `/sd` |
| `DELETE /files/<path>` | – | deletes a file or empty directory |
| `POST /wifi` | `{"ssid":"...","password":"..."}` | saves a new network and reconnects |
| `POST /ota` | firmware image (`firmware/build/holoneo.bin`) | updates over Wi-Fi and reboots; rolls back automatically if the new build never gets on Wi-Fi |
| `POST /reboot` | – | reboots |
| `GET /ws?token=...` | WebSocket | pushes events as JSON text frames, e.g. `{"type":"gesture","gesture":"WAVE"}`; **binary frames you send are shown as JPEG frames** (live streaming, up to 30 fps) |

Gestures: `WAVE` (short swipe, any direction), `HOVER` (hand held in front ~½ s — also shows
the info screen for 8 s), `UP` (bottom → top, experimental; some waves are reported as `UP`).

## Examples

```sh
curl -X POST -H "$A" --data "Hello, world" "$H/text?size=2&color=00e5ff"
curl -X POST -H "$A" --data-binary @photo.jpg $H/image          # progressive JPEGs are rejected
ffmpeg -i in.png -vf scale=320:240:force_original_aspect_ratio=decrease out.jpg   # convert first
curl -X POST -H "$A" --data-binary @sound.wav $H/audio
curl -H "$A" "$H/mic?seconds=5" -o mic.wav
curl -X POST -H "$A" --data-binary @firmware/build/holoneo.bin $H/ota
HOLO_TOKEN=$HOLO_TOKEN python3 tools/holo_events.py               # live gesture events

# animations: 320x240 MJPEG plays fastest (other sizes are scaled down by 1/2, 1/4, 1/8)
ffmpeg -i in.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=25" \
       -c:v mjpeg -q:v 6 -pix_fmt yuvj420p -an clip.avi
curl -X PUT -H "$A" --data-binary @clip.avi $H/files/data/anim/clip.avi
curl -X POST -H "$A" "$H/play?path=/data/anim/clip.avi&loop=1"
HOLO_TOKEN=$HOLO_TOKEN python3 tools/holo_stream.py video.mp4 --fps 25    # live stream from the PC
```

## Performance notes

- Full-screen 320×240 JPEG frames decode in ~15 ms with the SIMD decoder (`esp_new_jpeg`);
  other sizes use the generic decoder (~4× slower). Sending a frame to the panel takes ~33 ms
  (SPI at 60 MHz) and overlaps with decoding, so ~30 fps is the ceiling.
- Live streaming needs ~330 KB/s at 30 fps; the firmware enlarges the Wi-Fi/TCP buffers
  (in PSRAM) for that — the ESP-IDF defaults only allow ~150 KB/s.

Black is transparent in the reflection — dark backgrounds make content appear to float.
