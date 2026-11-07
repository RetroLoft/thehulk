# Internet on the Atari with STinG

TheHulk's ESP32 is on your Wi-Fi network. With the STinG TCP/IP stack and the driver
`CART_NET.STX` the Atari gets its own IP address there: it can ping, fetch web pages, use
FTP and be reached from other computers.

```text
program (PING, URLVIEW, HighWire, gapFTP, ...)
  -> STinG (IP, TCP, UDP, DNS)
  -> CART_NET.STX (Ethernet frames, ARP)
  -> cartridge port -> ESP32 -> Wi-Fi -> your router -> internet
```

The drives keep working at the same time: the SD card, the TNFS drives and the flash drive. There is no
separate network mode.

## 1. What you need

- Wi-Fi set up on TheHulk (`THEHULK.PRG` on the flash drive, `I:` by default, see
  [DRIVES.md](DRIVES.md)).
- `STINGINS.TOS` on the flash drive, next to `THEHULK.PRG`. It carries everything else:
  STinG 1.26 (Peter Rottengatter and Ronald Andersson, freeware), `TCP.STX` 1.41 by
  Christian Zietz ([chzsoft/sting](https://github.com/chzsoft/sting)), `UDP.STX`,
  `RESOLVE.STX`, `PING.PRG` and TheHulk's own `CART_NET.STX`.
- Any 16-bit Atari: ST, STE, Mega ST(E), TT, Falcon.

## 2. Choose an IP address for the Atari

The Atari uses the **same MAC address as the ESP32**, but its **own IP address**. STinG
has no DHCP, so you pick one yourself:

- It must be **free**: no other device may use it.
- Take one **outside the range your router hands out** (DHCP). A FRITZ!Box hands out
  `.20`–`.200` by default, so for `192.168.178.x` pick e.g. `192.168.178.50` or `.210`.
- Not the ESP32's own address. The installer shows it, and so does `THEHULK.PRG`.

### The four addresses you need

| | What it is | Example | Where to find it |
|---|---|---|---|
| **Atari IP address** | the Atari's own address | `192.168.178.50` | you choose it (above) |
| **Subnet mask** | which part of an address says "same network" | `255.255.255.0` | the installer suggests TheHulk's |
| **Router** (gateway) | the way to the internet; also your name server | `192.168.178.1` | the installer suggests TheHulk's |
| **Network address** | the name of your whole network, not a device | `192.168.178.0` | your IP address with the last number set to `0` (with mask `255.255.255.0`) |

Most home networks use mask `255.255.255.0`: all devices share the first three numbers
(`192.168.178.`), only the last one differs.

## 3. Install STinG

Start `STINGINS.TOS` from the flash drive (`I:\STINGINS.TOS` by default). It shows TheHulk's own Wi-Fi address and asks:

| Question | Default |
|---|---|
| Install on drive | the boot drive |
| Atari IP address | the one already set, else your network with `.50` |
| Subnet mask, gateway (router) | TheHulk's |
| DNS server | the gateway (almost always the router) |

It writes:

| File | |
|---|---|
| `AUTO\STING.PRG`, `AUTO\STING.INF` | STinG, and where its folder is (`C:\STING\`) |
| `STING\CART_NET.STX`, `TCP.STX`, `UDP.STX`, `RESOLVE.STX` | the modules |
| `STING\TOOLS\PING.PRG`, `PING.RSC` | to try it |
| `STING\DEFAULT.CFG` | STinG's settings, with `CART_IP`, `CART_MASK` and `NAMESERVER` |
| `STING\ROUTE.TAB` | the routes: your network directly, everything else via the router |

Restart. At start-up you should see

```text
CART_NET.STX 00.03: port TheHulk installed
CART_NET.STX: IP 192.168.178.50, mask 255.255.255.0, port active
```

When the port is active, TheHulk switches its bridge on (serial console `w`: *bridge on*).
If the Atari starts at the same moment as TheHulk, `CART_NET.STX` first waits up to 30
seconds for TheHulk's Wi-Fi (Esc skips that). In AP mode without a connection it does not
wait and is not installed (see [WEB.md](WEB.md)). After a firmware update, start
`STINGINS.TOS` again to get the newest `CART_NET.STX`.

**Again, to change the settings:** start `STINGINS.TOS` once more. It finds the
installation through `AUTO\STING.INF` and offers *1 = change the network settings only*
or *2 = install everything again*. Either way `DEFAULT.CFG` and `ROUTE.TAB` are updated,
not replaced: other STinG settings and the routes of other ports stay.

## 4. What the installer writes, in detail

**DEFAULT.CFG.** `CART_NET.STX` reads its address from

```text
CART_IP     = 192.168.178.50
CART_MASK   = 255.255.255.0
```

and switches its port on with it, so the STinG Port Setup CPX in XControl is not needed.
`NAMESERVER` is the name server, which turns names like `info.cern.ch` into IP addresses.
`THREADING` (milliseconds) is how often STinG polls the cartridge; the template has 20.
Smaller values react quicker and cost more CPU.

STinG 1.26 reads `DEFAULT.CFG` correctly only with CR LF line ends: with LF only it skips
the line after every setting. The installer always writes CR LF. Editing the file on a PC,
keep the CR LF line ends.

**ROUTE.TAB** is STinG's signpost: for every packet it looks up which way it has to go.
Each line is *network, mask, port, gateway*, separated by tabs; lines starting with `#`
are comments:

```text
192.168.178.0	255.255.255.0	TheHulk	0.0.0.0
0.0.0.0		0.0.0.0		TheHulk	192.168.178.1
```

- **Line 1, your own network:** every address that starts with `192.168.178.` is sent
  **directly**; gateway `0.0.0.0` means "no stop in between".
- **Line 2, everything else** (the internet): network `0.0.0.0` with mask `0.0.0.0`
  matches any address. It goes **via your router**.

STinG takes the first line that matches, so the general line comes last.

## 5. Try it

- **Ping the router:** `PING.PRG`, host `192.168.178.1` (the default `127.0.0.1` is the
  Atari itself and does not use the network). A FRITZ!Box drops about a third of the pings
  when they come ten a second, from any computer: that is the router, not TheHulk.
- **Ping the Atari from a PC:** `ping 192.168.178.50`. STinG answers by itself.
- **A web page:** URLVIEW from [Network tools for the Atari ST](https://github.com/RetroLoft/atari-net-tools),
  or HighWire. Only `http://`: most sites now send you on to `https://`, which these
  programs cannot do. `http://info.cern.ch/` works.
- **Weather:** WeaSTher.
- **FTP:** e.g. [gapFTP](https://atariuptodate.de/en/905/gapftp) (17 KB, command line):
  `GAPFTP.TTP ftp.funet.fi`.

## How it works

- The ESP32 catches every frame from Wi-Fi that is meant for the Atari's IP address (IPv4,
  or ARP asking for it) and keeps up to 32 of them. Everything else goes to the ESP32's own
  network stack as before (TNFS, the clock).
- `CART_NET.STX` asks for them from STinG's timer interrupt, up to 16 per poll, and only
  when TheHulk shows in its shared memory that frames are waiting. Frames from the Atari go
  out through the ESP32's Wi-Fi as they are.
- The commands use the same cartridge protocol as GEMDRIVE (0x0620 information, 0x0621 on/
  off, 0x0622 send, 0x0623 receive). GEMDRIVE marks the moments it is waiting for an answer;
  the driver then skips that poll, so a network poll never cuts into a disk call.

## Speed and limits

- Measured on a Falcon over the local network: a 1 MB HTTP download at **99 KB/s**
  (URLVIEW `-n`).
- The Atari **cannot reach the ESP32's own IP address** (such frames could not come back
  over Wi-Fi). Everything else on the network and the internet works.
- Some routers or mesh systems may not like two IP addresses on one MAC address. Home
  routers normally have no problem with it.
- STinG takes memory and a timer interrupt. Some games do not start with it loaded: start
  those without STinG.

## Troubleshooting

| Symptom | Check |
|---|---|
| *no TheHulk cartridge found* at start-up | Cartridge switched off (button 1 or console), or not seated well |
| *firmware without network function* | Firmware too old |
| *cannot pass frames (no Wi-Fi?)* | Wi-Fi not set up or not connected: `THEHULK.PRG` or the [web page](WEB.md) |
| *TheHulk is in AP mode (no Wi-Fi), not installed* | AP mode without a connection to your Wi-Fi: switch it off, see [WEB.md](WEB.md) |
| No line *IP ..., port active* at start-up | `CART_IP` missing in `DEFAULT.CFG` (start `STINGINS.TOS`), or LF line ends |
| *port NOT active (TheHulk refused)* | IP address equal to the ESP32's, or not a valid address/mask |
| Ping to the router fails | `ROUTE.TAB`, port active, console `w` (frames counted?) |
| Names do not resolve | `NAMESERVER` in `DEFAULT.CFG`, ping the router first |

The serial console key `w` shows the bridge: on/off, the Atari's IP address and counters
of frames to and from the Atari (dropped, refused, errors).
