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
| `POST /wifi` | `{"ssid":"...","password":"..."}` | saves a new network and reconnects |
| `POST /ota` | firmware image (`firmware/build/holoneo.bin`) | updates over Wi-Fi and reboots; rolls back automatically if the new build never gets on Wi-Fi |
| `POST /reboot` | – | reboots |
| `GET /ws?token=...` | WebSocket | pushes events as JSON text frames, e.g. `{"type":"gesture","gesture":"WAVE"}` |

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
```

Black is transparent in the reflection — dark backgrounds make content appear to float.
