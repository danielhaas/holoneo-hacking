# Getting into the Holoneo

All you need is a USB cable and a Linux/macOS/Windows PC. Nothing has to be opened.

## 1. Serial port

Plugging the device in shows a CH340 USB-serial bridge:

```
$ lsusb | grep 1a86
... ID 1a86:7523 QinHeng Electronics CH340 serial converter
$ ls /dev/ttyUSB*
/dev/ttyUSB0
```

On Linux your user needs to be in the `dialout` group (`sudo usermod -aG dialout $USER`, then log out and in).

The firmware prints nothing unprompted. Reset the board and it prints its full ESP-IDF boot log at **115200 baud**:

```
python3 tools/serial_log.py --reset --seconds 15
```

Any terminal works too (`picocom -b 115200 /dev/ttyUSB0`, then replug or toggle DTR/RTS).
There is no command shell on the serial port — it is log output only.

## 2. Back up the flash (do this first)

```
pip install esptool
esptool -p /dev/ttyUSB0 flash-id
esptool -p /dev/ttyUSB0 -b 921600 read-flash 0 0x1000000 holoneo_backup.bin   # ~4 minutes
```

`esptool` puts the chip into its ROM bootloader through the auto-reset lines; no button press is needed.

> ⚠️ The backup contains your Wi-Fi name and **password in plain text** (NVS partition).
> Keep it private.

Restore it at any time with:

```
esptool -p /dev/ttyUSB0 -b 921600 write-flash 0 holoneo_backup.bin
```

## 3. The network side (stock firmware)

- The device joins your Wi-Fi as a station **and** always runs its own access point
  `Holoneo` with **no password** (IP `192.168.4.1`).
- A settings web page without any login runs on port **8080** on both interfaces.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | settings page |
| GET | `/scan` | scan for Wi-Fi networks |
| POST | `/configure` | set Wi-Fi SSID/password (JSON) |
| GET | `/api/status` | connection status, SSID, RSSI, IP |
| GET | `/api/saved` | saved networks |
| POST | `/api/connect`, `/api/delete` | manage saved networks |
| POST | `/open_ai` | set OpenAI API URL + token |
| POST | `/weather` | set city, time zone, volume |

**Security notes:** anyone in range can join the open `Holoneo` network and reconfigure the device,
including the OpenAI key, which is stored on the device and sent over plain HTTP.
The stock firmware contacts `ntp.aliyun.com`, `api.m.taobao.com` (time), `ip-api.com` (geo-IP) and
`qweatherapi.com` (weather).

## 4. Built-in apps (stock firmware)

Switched with hand gestures over the sensor:

| App | Content |
|---|---|
| Video | `VIDEO/*.avi` on the SD card — AVI with MJPEG video (+ optional audio) |
| Pictures | `PICTURE/*.jpg` on the SD card |
| Music | sound spectrum from the microphones |
| Weather | QWeather, city set via web page |
| Open AI | OpenAI Realtime voice assistant over WebRTC (needs your key) |
| Settings | — |

Making a compatible video (adjust size to your content; the screen is 240 × 320):

```
ffmpeg -i in.mp4 -vf "scale=240:320:force_original_aspect_ratio=decrease,pad=240:320:(ow-iw)/2:(oh-ih)/2" \
       -c:v mjpeg -q:v 5 -r 20 -c:a pcm_s16le -ar 16000 -ac 1 VIDEO/clip.avi
```

(Untested against the stock player yet — the format is inferred from the firmware.)

## 5. Reverse engineering

See [stock-firmware.md](stock-firmware.md) for how the pin map was recovered from the image.
