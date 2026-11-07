// The web page and its HTTP API (docs/WEB.md): the state of TheHulk, the cartridge on or
// off, and every setting THEHULK.PRG has, plus the device name. In setup mode (TheHulk's
// own network, for a meeting away from home) the whole page is there too, at 192.168.4.1.
#pragma once

void web_start(void);           // after wifi_start
