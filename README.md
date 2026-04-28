# ESP32 WiFi Pocket — NAT Router

Miniature WiFi router for ESP32-S3 with web-interface. Creates an access point with a unique password, connects to an external WiFi network, and shares internet via NAT.

## Features

- Unique AP password generated on first boot (MAC-based), printed to console
- Web-interface at `http://192.168.4.1` with two tabs:
  - **WiFi Setup** — scan, select and connect to external networks
  - **Settings** — view/change AP password, factory reset
- NAT routing — internet sharing for up to 7 clients
- Auto-sync AP channel with external network
- DNS forwarding from ISP
- Settings saved to NVS (survive reboots)
- Auto-reconnect on connection loss

## Supported Chips

| Chip | Status |
|:---|:---|
| ESP32 | Should work |
| **ESP32-S3** | **Tested** |
| ESP32-C3 | Should work |
| ESP32-C5 | Should work |
| ESP32-C6 | Should work |
| ESP32-S2 | Not supported (no AP+STA) |

## Quick Start

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py menuconfig
```

In menuconfig enable:
- `Component config → LWIP → IP forwarding` — `[*]`
- `Component config → LWIP → Enable NAT` — `[*]`

Then flash:

```bash
esptool.py -p /dev/ttyUSB0 erase_flash
idf.py -p /dev/ttyUSB0 build flash monitor
```

## First Boot

After flashing, the console will show:

```
==============================
AP SSID:     Pocket-1A2B
AP Password: C3F81A2B
>>> NEW PASSWORD GENERATED <<<
==============================
```

Connect to the `Pocket-XXXX` network with the password shown, then open `http://192.168.4.1` to configure external WiFi.

## Default Password

On first boot, the AP password is generated from the device MAC address.  
If you don't have serial console access, you can figure it out from the AP name:

**Example:**
```
AP SSID: Pocket-9435   ← visible in your WiFi list
MAC:      ...:21:94:35 (hidden)
Password: D9219435     ← last 4 bytes of MAC: D9:21:94:35
```

The SSID `Pocket-9435` uses the last 2 bytes of the MAC (`94:35`).  
The password uses the last 4 bytes of the same MAC (`D9:21:94:35` → `D9219435`).

> ⚠️ Change the password via web-interface (`Settings` tab) after first connect.

## License

GPL v3 © Ivan Svarkovsky, 2026
