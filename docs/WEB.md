# TheHulk's web page

TheHulk has a web page for its state and all its settings, with no program needed on the
Atari. It works from a phone, a tablet or a computer on the same network.

- At home: **http://thehulk.local/**, or the IP address that `THEHULK.PRG` and the router
  show. With another device name the address changes: "Hulk Falcon" becomes
  `http://hulk-falcon.local/`.
- In AP mode: TheHulk's own network, at **http://192.168.4.1/** (see below).

There is no password. The page is meant for a private home network.

## What is on it

**The front page**
- The state: the Atari on or off, the Wi-Fi network and signal, the address, and the
  firmware version (from `version.txt`).
- **Mode**: switches the cartridge on or off, like a short press on button 1 (see
  [The cartridge switched off](#the-cartridge-switched-off)). **Cartridges...** opens the
  [cartridge emulator](CARTRIDGE.md). The floppy emulator follows later.

**Settings** (the gear at the top right)
- **Device name**: tells several cartridges apart. The name is used:
  - on the page;
  - as the host name (`thehulk.local`);
  - as the name of the own network in AP mode.
- **Drives**: the same slots as in `THEHULK.PRG`.
  - Every drive has an on/off switch, which is saved at once.
  - **Edit** opens the drive. The letter, the type (SD card or TNFS), the folder on the
    card, and the server, port (16384 by default) and mount can all be changed.
  - When a folder is not on the SD card, the page offers to create it.
  - A letter after `P:` gives a warning (see [DRIVES.md](DRIVES.md)).
- **Flash drive**: its letter.
- **Wi-Fi**: the network name and password, DHCP or fixed addresses, and the country.
  - **Search networks** lists the networks nearby.
  - The password is never shown. It only changes when you type a new one.
- **Clock**: time from the internet (NTP) on or off, the server, and the UTC offset. There
  is no automatic summer time.
- **AP mode**: shows the state, with a button to switch it on or off.

Changes in Settings are only kept after **Save**; **Revert** drops them. The last save
wins: there is no check for someone else changing the settings at the same time.

Saved settings take effect after the next **reset of the Atari**. Wi-Fi settings are tried
at once: TheHulk connects again, so the page may lose it for a moment.

When a question needs the Atari switched off first, such as switching AP mode on or saving
Wi-Fi settings, the page asks in a window with **SWITCH OFF YOUR ATARI**.

**When something is wrong**
- **OFFLINE**: a window over the whole page when TheHulk does not answer for a few seconds.
  This happens when it restarts, or when the phone switched to another network by itself.
  The window goes by itself when TheHulk answers again.
- A red bar **TheHulk is not connected to Wi-Fi** when TheHulk itself has no connection.
  TNFS drives, STinG and the time do not work then.

## AP mode

In AP mode TheHulk opens its own open Wi-Fi network, **"&lt;device name&gt;-XXXX"**, for
instance `TheHulk-9895`. The whole page is at http://192.168.4.1/. This is for a meeting
away from home, or to fix the Wi-Fi settings.

**Switching it on**
- Hold **button 1 for 10 seconds**. The button is on the board, and can also be pressed
  with tweezers.
- Or use the page, under Settings, AP mode.
- Or press `a` on the serial console.
- It also comes on by itself when no Wi-Fi network is set.

**Switching it off**
- Use the page: the bar at the top, or Settings, AP mode.
- Or hold button 1 for 10 seconds again.
- Or press `a` on the serial console.

**How it works**
- AP mode comes **next to** the home network, not instead of it.
- At home, TheHulk stays connected to your Wi-Fi: TNFS, STinG and the time keep working.
  The own network then uses the channel of your router.
- Without your Wi-Fi, at a meeting, there is only the own network.
- When AP mode is on and there is no connection, TheHulk stops searching for your network,
  because the search would disturb the own network. Switch AP mode off to search again.
- AP mode is **remembered** over a restart: the Atari powers TheHulk, so it would be gone at
  every power-on. After a restart, TheHulk first tries your Wi-Fi for 15 seconds, then opens
  the own network.
- It stays on until you switch it off, or until you save new Wi-Fi settings. New Wi-Fi
  settings are tried at the next start, without AP mode.

**On the Atari**
- TNFS drives show `NET_ERR.TXT` with the reason (see [DRIVES.md](DRIVES.md)).
- The STinG driver says *TheHulk is in AP mode (no Wi-Fi), not installed*. It does not wait
  for Wi-Fi.

**Phones**
- The own network also answers name lookups. A phone then sees a "sign in to network"
  page, and that page opens TheHulk's page.
- Many phones switch back to a known network that has internet. If the page goes OFFLINE,
  check which network the phone is on.

The own network is open: everyone nearby can open the page and change the settings, but
the Wi-Fi password is never shown. Switch AP mode off when you do not need it.

## The status LED

Short events go first:

| Colour | Meaning |
|---|---|
| red, 2 s | an error |
| green, short | a drive at work |
| purple, short | an SD card was put in |

Then the states:

| Colour | State | Alone |
|---|---|---|
| green | the [cartridge emulator](CARTRIDGE.md) is in use | blinks |
| white | AP mode | blinks |
| yellow | the cartridge is switched off | stays on |
| blue | no Wi-Fi (yet) | blinks |

When two or three states are active, their colours take turns, half a second each: for
example white and blue (AP mode without Wi-Fi), or white and yellow (AP mode with the
cartridge off). With no state active, the LED is off.

## The cartridge switched off

A short press on button 1 (under 1.5 s), the page, or the console keys `d` and `e` switch
the cartridge off and on. While the cartridge emulator is in use, the short press on button 1
goes back to GEMDRIVE instead.

When it is off:
- From the next reset, GEMDRIVE does not install itself.
- From the next power-on, the Atari does not see the cartridge at all.

This is useful to start a game without TheHulk while it stays in the Atari. The setting is
kept over a restart.

## The HTTP API

The page uses a small JSON API, which other programs can use too. Calls that change
something need `Content-Type: application/json`, otherwise the answer is `415`. An error
comes back as `{"error": "<CODE>", "message": "<text>"}`.

| Call | What it does |
|---|---|
| `GET /api/v1/status` | name, hostname, version, `wifi` {ssid, ip, rssi, setup, setup_ssid, via_setup}, atari_on, `cartridge` {on, emulator, image, busy, running, load_failed, error} |
| `PUT /api/v1/cartridge` | `{"on": true}` or `false` |
| `GET /api/v1/settings` | name, hostname, flash_letter, `drives` [{state: enabled/disabled/empty, letter, type: sd/tnfs, name, sd_path, host, port, mount}], `network` {ssid, password_set, auth_mode, dhcp, ip, netmask, gateway, dns, country}, `clock` {enabled, ntp_server, utc_offset} |
| `PUT /api/v1/settings` | the same object. Everything is checked first, and an error changes nothing. `network.password` only when it changes, `network.clear_password` to remove it. |
| `POST /api/v1/sd-folder` | `{"path": "/HD", "create": false}`; answer `status`: there, missing, created, failed, is_file, no_card |
| `GET /api/v1/setup` | the AP-mode state |
| `GET /api/v1/setup/scan` | the networks nearby (takes a few seconds) |
| `PUT /api/v1/setup/wifi` | `{"ssid": "...", "password": "..."}`: only the network and password |
| `POST /api/v1/setup/start` | AP mode on |
| `POST /api/v1/setup/leave` | AP mode off |
| `PUT /api/v1/mode` | `{"mode": "gemdrive"}` or `{"mode": "emulator", "image": "XENON.STC"}`; the status shows `busy` until it is in the cartridge |
| `GET /api/v1/carts` | emulator, image, card, `files` [{name, size, kind: normal/diagnostic/unknown/bad}] |
| `POST /api/v1/carts/upload?name=X.STC` | the file as the body, `Content-Type: application/octet-stream` |
| `DELETE /api/v1/carts?name=X.STC` | removes an image from the card |
| `PUT /api/v1/carts/rename` | `{"from": "A.STC", "to": "B.STC"}` |
| `GET /api/v1/carts/catalog` | the list of roms.sidecartridge.com |
| `POST /api/v1/carts/download` | `{"url": "http://roms.sidecartridge.com/...", "name": "X.STC"}`; only from that site |

Through the own network, any other address is redirected to http://192.168.4.1/.
