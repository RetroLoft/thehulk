# Leesfouten op de Falcon030: rapport

Stand: 4 november 2026, gateware v20 (`fpga/rtl`, meldt zich als versie 20).

## Samenvatting
De Falcon030 las de cartridge fout, vooral woorden met veel enen. Andere machines hadden daar geen last van.
De fouten zijn met gateware v20 vrijwel verdwenen: één klok minder van adres naar data.

**Niet helemaal opgelost.** Op 4,7 miljoen directe reads blijven 8 fouten over, en een kopie van 1,47 MB had
nog 1 fout woord. Gebruik de Falcon dus nog niet voor belangrijke kopieën. Op de andere machines geeft v20 geen
enkele fout.

## Resultaat

`CARTTEST.PRG` op de Falcon (TOS 4.04), normale firmware met GEMDRIVE, start vanaf harde schijf:

| | v14 | v20 |
|---|---:|---:|
| `Fread`, 196608 woorden (v14: 180224) | 2631 | 0 |
| Long, 1,57 miljoen woorden (v14: 1,44 miljoen) | 16478 | 4 |
| Word, idem | 1789 | 3 |
| Byte, idem | 3469 | 1 |
| Foute woorden in patroon `FFFF` | 8677 | 0 |
| Mislukte `Fread`-blokken | 2 | 0 |

Kopie van `S.ST` (1474560 bytes), vergeleken met het origineel op de server:

| | Foute woorden |
|---|---:|
| v14 (eerste bord, ander moment) | 527 |
| v20 | 1 |

`CARTTEST.PRG` met v20 op de andere machines, elk ruim 4,9 miljoen woorden:

| Machine | Fouten |
|---|---:|
| TT, TOS 3.06 | 0 |
| ST, TOS 1.04 | 0 |
| ST, EmuTOS | 0 |
| STE, TOS 1.62 | 0 |
| Mega STE, TOS 2.06 | 0 |

Op de Falcon is daarna toegevoegd: een checksum per leesblok met opnieuw kopiëren, en de lussen die ROM3 lezen
draaien vanuit RAM. Resultaat op de Falcon: drie `CARTTEST`-runs zonder foute woorden, één keer `Fread` -37
(ongeldige handle) in de tweede run, een kopie van `S.ST` identiek. Een eerdere mislukte kopie viel samen met een
bewegende cartridge en een USB-reset. De STE (TOS 1.62) blijft foutloos met deze versie.

## Oorzaak
De gateware liep van adres naar data via drie stappen: adresregister, SPRAM, uitgangsregister. Dat kostte
64 ns (simulatie). In een Falcon-cyclus is dat te lang: `/ROM3` is daar 230-250 ns laag tegen 310-330 ns op de
TT, en bij eerder onderzoek veranderden adreslijnen op de Falcon nog aan het begin van de cyclus. De Atari zag
dan nog het vorige woord, vaak uit een deel van de cartridge met `0000`. Dat verklaart waarom vooral enen
wegvielen.

Deze verklaring past bij de metingen, maar is niet rechtstreeks gemeten: er is geen bruikbare scoopmeting.

## De metingen die daartoe leidden
Alle met gateware v14, tenzij anders vermeld, en `FFFF` gelezen met `CARTCONS.PRG` vanaf harde schijf.

| Opstelling | Long-fouten op 358400 |
|---|---:|
| Vaste waarden uit de FPGA, zonder SPRAM (`DIAG_CONST`) | 0 |
| Patronen in heel ROM3, ROM4 `0000` of `FFFF` | 0 |
| Idem, met statuspolling of SPRAM-writes elke milliseconde | 0 |
| Idem, met SPRAM-reads elke milliseconde | 0 (2 fouten in wisselpatroon) |
| Idem, cartridge-magic in ROM4 zonder programma | 0 |
| Normale firmware met GEMDRIVE, ROM3 grotendeels `0000` | 76506 |
| Normale firmware, zonder wifi | 76317 |
| Normale firmware, GEMDRIVE stopt meteen, geen commando's | alle `FFFF` fout |
| Normale firmware, leeg image, geen GEMDRIVE | 77043 |
| Patroonfirmware, ROM3 alleen `FFFF` op `$FB8000`, rest `0000` | ongeveer gelijk |
| **Hetzelfde met v20** | **0** |

Uit deze reeks volgt ook wat de oorzaak **niet** is: buffer U9 en `/OE`, ICD, USB, contacten, wifi, GEMDRIVE-code
en de print zelf (twee borden gedragen zich gelijk). Een nieuwe print lijkt hiervoor niet nodig.

## De wijziging
In `fpga/rtl/top.v` gaat de SPRAM-uitgang direct naar de pinnen. `d_q` houdt het woord alleen nog vast als de
SPI de SPRAM gebruikt. De vertraging na `/ROM` laag gaat in de simulatie van 64 naar 42 ns. De timing haalt
52 MHz (48 MHz nodig). De simulatie slaagt.

Eerdere pogingen (v16, v17, v18, v19) maakten het slechter. Bij v18 en v19 kwamen commando's verminkt binnen. Die
reden is onbekend.

## Wat nog open is
0. **Vier bommen.** Voor de RAM-lussen stopte `CARTTEST` eens op `$FA1E42`, in de lus die een schrijfblok verstuurt:
   een verkeerd gelezen instructie. Niet meer gezien sinds de RAM-lussen, maar daarvoor zijn er nog te weinig runs.
   Andere GEMDRIVE-code draait nog steeds van de cartridge.
1. **Restfouten.** Bij `CARTTEST` alle 8 op `$FB2010`, verwacht `0000` na een `FFFF`, gekregen onder meer `FF00`.
   Bij de kopie op `$FB3D54`: verwacht `0000`, na `FFFF`, gekregen `4000`. Hier blijven enen dus staan: de andere
   richting dan het hoofdprobleem. Oorzaak onbekend.
2. **Mislukte kopie.** De eerste kopie van `S.ST` met v20 stopte met een foutmelding na ongeveer 900 KB, de tweede
   lukte. De tekst van de melding is niet genoteerd en de oorzaak is niet vastgesteld.
3. **`Fread` -37.** Zeldzaam, ook al met v14 gezien. Vermoedelijk een verminkt commando of slecht contact; de firmware
   logt nu bij een mislukte `Fread` het binnengekomen commando.
4. **Schade aan bord 1.** Dat bord geeft geen 5 V meer door en trok de DevKit in brown-outs. Niet doorgemeten.

## Testen
- `atari/carttest/carttest.c`: `CARTTEST.PRG`, schrijft en leest een testbestand op `N:\FALCON` en leest de buffer
  direct. Start het vanaf harde schijf. Uitslag in `N:\FALCON\CARTTEST.TXT`.
- `atari/carttest/cartcons.c`: `CARTCONS.PRG`, leest vaste data, zonder iets te schrijven op de cartridge.
- Bouwopties in `esp32/main/CMakeLists.txt`: `HULK_DIAG_PATTERN`, `HULK_DIAG_HIGH`, `HULK_NO_WIFI`, `HULK_NO_LOG`.
- Gateware met vaste waarden: `make -B YOSYS_DEFS="-D DIAG_CONST"` in `fpga/`.
- Meetuitslagen en de patches van de mislukte pogingen staan lokaal in `esp32/build-roms/`, buiten git.
