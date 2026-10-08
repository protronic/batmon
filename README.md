# batmon – Batterie-CAN-Monitor für das STM32H573I-DK

batmon ist eine Zephyr-RTOS-Anwendung, die am CAN-Bus zwischen Batteriespeicher
(BMS) und Wechselrichter mithört, das Protokoll und den Hersteller-"Flavor"
erkennt (Pylontech-Original, PACE/FM-Solar, Seplos, JK, Felicity, BYD,
SMA-Dialekt, Victron-Erweiterung, Pylontech-HV, Growatt-/GoodWe-Marker), den
Batteriezustand dekodiert, ihn auf dem Display des Discovery-Kits anzeigt und
per MQTT über die Ethernet-Buchse weiterreicht – generisch (mit Home-Assistant-
Discovery) oder im ThingsBoard-Geräteformat.

Zusätzlich oder alternativ fragt batmon das BMS über dessen **RS485-Port** ab
(Pylontech-, PACE- und Seplos-ASCII-Protokoll, JK-BMS, Seplos-V3-Modbus) und
liefert damit Zellspannungen, Temperaturen, Kapazität, Zyklen, Modell und
Seriennummer, die am CAN nicht zu sehen sind. Auf Wunsch hört es dort nur mit,
wenn der Wechselrichter den Speicher selbst per RS485 abfragt.

* Board: **ST STM32H573I-DK** (Cortex-M33, 240×240-TFT, Ethernet, FDCAN)
* Zephyr **v4.4.2**, LVGL 9, Zephyr-MQTT-Client
* Standard: reiner **Listen-only-Betrieb** – batmon sendet nichts auf den Bus
* Bench-Modus: batmon spielt den Wechselrichter (Heartbeat 0x305), damit ein
  einzelner Speicher auf dem Tisch weiterredet

## Hardware-Anschluss

### CAN

Das Discovery-Kit hat keinen CAN-Transceiver an Bord. batmon nutzt **FDCAN2**
am Arduino-Header mit einem externen 3,3-V-Transceiver-Breakout
(SN65HVD230, TJA1051T/3, MCP2562FD o. ä.):

| Signal | MCU-Pin | Arduino-Header | Transceiver |
|--------|---------|----------------|-------------|
| FDCAN2_RX | PB5 | D3 | RXD |
| FDCAN2_TX | PB6 | D15 (Aufdruck SCL) | TXD |
| 3,3 V | – | 3V3 | VCC |
| GND | – | GND | GND |

Batterieseite (Pylontech-Belegung, von PACE/FM-Solar, Seplos, Dyness u. a.
übernommen): RJ45-Buchse "CAN", **Pin 4 = CAN-H, Pin 5 = CAN-L**. GND der
Batterie-CAN-Schnittstelle mit dem Board-GND verbinden.

Terminierung: Hängt batmon an einen bestehenden Bus (Batterie ↔ Wechselrichter),
keinen weiteren Abschlusswiderstand setzen. Im Bench-Betrieb (nur Batterie und
batmon) 120 Ω am Transceiver aktivieren – die meisten Speicher haben intern
bereits 120 Ω.

Alternativ ist FDCAN1 auf PA11/PA12 (USB-C-Datenleitungen) nutzbar, siehe
Kommentar in `boards/stm32h573i_dk.overlay`.

### RS485 (optional)

USART3 liegt ebenfalls auf dem Arduino-Header. Ein 3,3-V-RS485-Transceiver
(MAX3485, SP3485, SN65HVD72 oder ein fertiges Modul) wird so angeschlossen:

| Signal | MCU-Pin | Arduino-Header | Transceiver |
|--------|---------|----------------|-------------|
| USART3_TX | PB10 | D1 | DI |
| USART3_RX | PB11 | D0 | RO |
| Richtungsumschaltung | PG15 | D2 | DE und /RE (gebrückt) |
| 3,3 V / GND | – | 3V3 / GND | VCC / GND |

Module mit automatischer Richtungsumschaltung brauchen D2 nicht; dann die Zeile
`de-gpios` im Overlay entfernen. Batterieseite:

| Speicher | RJ45-Buchse "RS485" | Baudrate |
|----------|---------------------|----------|
| Pylontech US2000/US3000/US5000, Force L | Pin 7 = A, Pin 8 = B (alternativ Pin 1 = B, Pin 2 = A), GND Pin 3/6 | 115200 (DIP 1 unten) oder 9600 |
| PACE-BMS (FM-Solar, Gobel, SOK ...) | meist Pin 1 = B, Pin 2 = A – Handbuch prüfen | 9600 |
| Seplos V2 / V3 | Pin 1 = B−, Pin 2 = A+, GND Pin 3/6 | 9600 bzw. 19200 (V3, Modbus) |
| JK-BMS | eigener RS485-Adapter von JK nötig (der "RS485"-Port am BMS ist TTL) | 115200 |

GND mitführen, bei fehlender Antwort A und B tauschen. Die Abschlusswiderstände
sind bei den kurzen Leitungen am Speicher meist unkritisch; bei langen Leitungen
120 Ω am Transceiver aktivieren. Ohne Konfiguration probiert batmon alle
Protokolle, Baudraten und Adressen durch und zeigt das Ergebnis mit `batmon
rs485 status`.

Hängt der Wechselrichter selbst am RS485-Port (Pylontech-RS485-Modus, Growatt,
Deye, Voltronic ...), wird batmon parallel angeklemmt und mit
`CONFIG_BATMON_RS485_LISTEN_ONLY=y` gebaut: es sendet dann nichts, sondern
dekodiert die Anfragen des Wechselrichters und die Antworten des BMS.

### Ethernet und Display

RJ45 und Display sind auf dem Kit vorhanden. Die IP-Adresse kommt per DHCP.

## Bauen und flashen

Voraussetzungen: `west`, Zephyr SDK (≥ 0.17, getestet mit 1.0.1), Python ≥ 3.12,
CMake ≥ 3.20, Ninja. Das Repository ist ein West-Manifest (T2-Topologie): Zephyr
und die nötigen Module (hal_stm32, cmsis, lvgl) werden daneben ausgecheckt.

```sh
mkdir batmon-ws && cd batmon-ws
git clone https://github.com/protronic/batmon.git
west init -l batmon
west update --narrow -o=--depth=1
west build -b stm32h573i_dk batmon
west flash            # STM32CubeProgrammer; alternativ --runner pyocd
```

Eigene Einstellungen (Broker, Device-ID, Bench-Modus) kommen in eine lokale
Konfigurationsdatei, die nicht eingecheckt wird:

```sh
cat > batmon/local.conf <<'CONF'
CONFIG_BATMON_DEVICE_ID="speicher-garage"
CONFIG_BATMON_MQTT_BROKER_HOST="192.168.178.10"
CONFIG_BATMON_MQTT_USERNAME="batmon"
CONFIG_BATMON_MQTT_PASSWORD="geheim"
CONF
west build -b stm32h573i_dk batmon -- -DEXTRA_CONF_FILE=local.conf
```

Nach dem Flashen meldet sich die Shell auf dem ST-LINK-VCP (115200 8N1).

## Konfiguration (Kconfig, `west build -t menuconfig`)

| Option | Standard | Bedeutung |
|--------|----------|-----------|
| `BATMON_DEVICE_ID` | `batmon` | MQTT-Client-ID, Topic-Bestandteil, Anzeige |
| `BATMON_CAN_BITRATE` | 500000 | Bitrate; Pylontech-Familie immer 500 kbit/s |
| `BATMON_CAN_LISTEN_ONLY` | y | kein ACK, kein Senden – für den Betrieb am Produktivbus |
| `BATMON_BENCH_HEARTBEAT` | n | Wechselrichter-Heartbeat 0x305 jede Sekunde (nur ohne Listen-only) |
| `BATMON_RAW_LOG` | n | jeden empfangenen Rahmen loggen (auch zur Laufzeit: `batmon raw on`) |
| `BATMON_MQTT` | y | MQTT-Anbindung |
| `BATMON_MQTT_BROKER_HOST` / `_PORT` | 192.168.1.10 / 1883 | Broker (Hostname oder IPv4) |
| `BATMON_MQTT_USERNAME` / `_PASSWORD` | leer | Login; bei ThingsBoard der Device-Access-Token als Username |
| `BATMON_MQTT_INTERVAL_SEC` | 5 | Sendeintervall |
| `BATMON_MQTT_MODE_GENERIC` / `_THINGSBOARD` | generisch | Topic-/Payload-Schema |
| `BATMON_MQTT_TOPIC_PREFIX` | `batmon` | Präfix im generischen Modus |
| `BATMON_HA_DISCOVERY` | y | Home-Assistant-Discovery (generischer Modus) |
| `BATMON_UI` | y | Statusbildschirm (LVGL) |
| `BATMON_RS485` | y (wenn Overlay-Knoten vorhanden) | RS485-BMS-Abfrage |
| `BATMON_RS485_LISTEN_ONLY` | n | nur mithören, nie senden |
| `BATMON_RS485_POLL_MS` | 2000 | Abfrageintervall |
| `BATMON_RS485_BAUD` | 0 | feste Baudrate (0 = 9600/19200/115200 probieren) |
| `BATMON_RS485_ADDR` | -1 | feste Pack-Adresse (-1 = 2/1/0 probieren) |
| `BATMON_RS485_FLAVOR_*` | AUTO | Protokoll fest vorgeben statt zu erkennen |
| `BATMON_JK_CURRENT_LEGACY` | n | JK-BMS mit alter Strom-Kodierung |

## MQTT

### Generischer Modus

| Topic | Inhalt |
|-------|--------|
| `batmon/<id>/state` | Telemetrie-JSON (unten), alle `BATMON_MQTT_INTERVAL_SEC` |
| `batmon/<id>/status` | `online` / `offline` (retained, Last Will) |
| `homeassistant/sensor/<id>_<key>/config` | Discovery für SoC, SoH, Spannung, Strom, Temperatur, CVL, CCL, DCL, Protokoll, Hersteller |

Beispiel (PACE-BMS, Wechselrichter-Heartbeat gesehen):

```json
{"ts":5000,"link":true,"frames":30,"rate":7.4,
 "proto":{"family":"pylon_lv","vendor":"pace",
          "flavor":"PACE-BMS (FM-Solar/DelongTop, Gobel, SOK ...)","mfr":"PACE",
          "conf":100,"hints":"0x343",
          "ids":["351:5","355:5","356:5","359:5","35c:5","35e:5","305:1"]},
 "inv":{"seen":true,"ident":"","hb":1},
 "pack":{"u":49.20,"i":-3.2,"t":23.5,"soc":78,"soh":100,
         "cvl":53.20,"dvl":47.00,"ccl":100.0,"dcl":100.0,
         "charge_en":true,"discharge_en":true,"force_charge":false,"full_req":false,
         "modules":2,"prot_raw":"0000","alarm_raw":"0000","prot":[],"alarm":[]}}
```

Bei Victron-Erweiterung bzw. Pylontech-HV kommen die Objekte `cells` (min/max
Zellspannung und -temperatur mit Zellnamen), `mod` (Module online/blockiert/
offline) und `info` (Chemie, Hardware-/Softwareversion, Kapazität) hinzu.
Strom: positiv = Laden.

Mit RS485 kommt das Objekt `serial` dazu:

```json
"serial":{"flavor":"pylontech_ascii","desc":"Pylontech-RS485 US2000C Pylon",
 "phase":"polling","link":true,"baud":115200,"ver":"20","addr":2,
 "model":"US2000C","mfr":"Pylon","sw":"1.2","sn":"PPTBH0123456789A",
 "u":49.20,"i":-3.20,"soc":79.0,"soc_derived":true,"remain_ah":39.50,"full_ah":50.00,
 "cycles":5,"cells":[3.300,3.300,...],"cell_min_mv":3300,"cell_max_mv":3300,
 "cell_min_idx":1,"cell_max_idx":1,"cell_diff_mv":0,"temps":[25.0,25.0,25.0,25.0,25.0],
 "t_max":25.0,"cvl":53.20,"dvl":47.00,"ccl":25.0,"dcl":25.0,
 "charge_en":true,"discharge_en":true,"force_charge":false,"full_req":false,
 "sensor_warn":0,"status_raw":"0000001000000000","ok":123,"err":2,"timeouts":1,"crc_err":0}
```

`soc_derived` zeigt an, dass der SoC aus Rest- und Vollkapazität berechnet wurde
(Pylontech und PACE senden ihn über RS485 nicht direkt). Im ThingsBoard-Modus
erscheinen dieselben Werte flach mit Präfix `rs_` (`rs_u`, `rs_soc`,
`rs_cell_diff_mv`, `rs_model` ...).

### ThingsBoard-Modus

`CONFIG_BATMON_MQTT_MODE_THINGSBOARD=y`, Username = Access-Token des Geräts.

| Topic | Inhalt |
|-------|--------|
| `v1/devices/me/telemetry` | flaches JSON: `uptime_s, link, frames, family, vendor, flavor, mfr, conf, inv_seen, u, i, t, soc, soh, cvl, dvl, ccl, dcl, charge_en, discharge_en, force_charge, modules, prot_raw, alarm_raw` |
| `v1/devices/me/attributes` | einmalig nach dem Verbinden: Firmware, Board, Device-ID |

## Shell

```
batmon status        dekodierter Zustand, Link, Protokoll, Netz
batmon ids           alle gesehenen CAN-Identifier mit Zähler und Alter
batmon json [flat]   Telemetrie-JSON (verschachtelt oder ThingsBoard-flach)
batmon raw on|off    jeden Rahmen loggen
batmon hb on|off     Bench-Heartbeat 0x305 (nur ohne BATMON_CAN_LISTEN_ONLY)
batmon reset         Erkennung und Zustand löschen
batmon publish       sofort per MQTT senden

batmon rs485 status  erkanntes RS485-Protokoll, Modell, Messwerte, Zähler
batmon rs485 cells   alle Zellspannungen
batmon rs485 probe   Erkennung neu starten
batmon rs485 cmd 42 02        Kommando (CID2 hex, INFO hex) mit erkannter VER/ADR senden
batmon rs485 send ~2002464F0000FDA2 [baud]   Rohes ASCII-Frame (Prüfsumme muss stimmen) bzw. Hex-Bytes senden
batmon rs485 last    letzte Antwort hex/ASCII
```

Dazu die Zephyr-Shells `can`, `net` (z. B. `net iface`, `net ping`) und
`device list`.

## Display

Seite 1 (CAN): Kopfzeile mit Device-ID und CAN-Link, großer SoC mit Balken,
Spannung/Strom/Temperatur, Lade- und Entladegrenzen, Freigabeflags (bei aktiver
Protection rot "SCHUTZ AKTIV"), erkannter Protokoll-Flavor mit Hersteller und
Konfidenz, unten IP-Adresse, MQTT-Status und Rahmenzähler.

Seite 2 (RS485): erkanntes Protokoll mit Modell und Hersteller, Spannung/Strom,
SoC/SoH, Zellspannungsbereich mit Differenz und Zellnummern, höchste Temperatur,
Kapazität und Zyklen, Limits bzw. Statusbytes, Zähler.

Der User-Taster wechselt die Seite; ohne Tastendruck zeigt batmon die Seite,
auf der gerade Daten ankommen. Die blaue LED leuchtet dauerhaft, solange
CAN-Rahmen eintreffen, und blinkt sonst.

## Protokolle und Erkennung

Rahmenbelegung, Bitnamen, Hersteller-Tabelle und die Erkennungsheuristik sind in
[docs/protocols.md](docs/protocols.md) beschrieben. Kurzfassung:

| Familie | Kennzeichen |
|---------|-------------|
| `pylon_lv` | 0x351/0x355/0x356 (+0x359, 0x35C, 0x35E); Original-Pylontech mit `PN`-Marker, sonst Emulation mit Hersteller aus 0x35E |
| `sma_lv` | zusätzlich 0x35A/0x35F, ohne 0x35C/0x359 (BYD/LG an SMA) |
| `victron_ext` | zusätzlich 0x370–0x379 (Name, Zell-Min/Max, Module, Kapazität) |
| `pylon_hv` | 29-Bit-IDs 0x4210–0x4290 (Force H1/H2, Powercube) |
| `growatt_lv` / `goodwe_lv` | 0x311/0x313 bzw. 0x455/0x456 (nur Erkennung) |

RS485-Seite (`serial.flavor`):

| Flavor | Erkennung | gelieferte Werte |
|--------|-----------|------------------|
| `pylontech_ascii` | antwortet auf VER 20 / ADR 2, Hersteller `PYLON` oder Modell US2000/US3000/US5000 | Zellen, Temperaturen, U/I, Rest-/Vollkapazität, SoC (berechnet), Zyklen, Limits und Freigaben (0x92), Seriennummer |
| `pace_ascii` | VER 25 oder Hersteller `PACE` / Modell P16S… | Zellen, Temperaturen, U/I, Kapazitäten, SoC (berechnet), Zyklen |
| `seplos_ascii` | Hersteller `SEPLOS` / Modell 1101-… oder Layout-Marker 0x0A | Zellen, Temperaturen, U/I, Kapazitäten, SoC, SoH, Zyklen, Port-Spannung |
| `generic_ascii` | YD/T-Frames ohne bekannten Herstellerstring, Layout wird geraten | wie oben, je nach Layout |
| `jk_rs485` | Antwort auf `4E 57 … 06` | Zellen, MOS-/Sensortemperaturen, U/I, SoC, Zyklen, Alarm-/Statusbits, Modell, Firmware |
| `seplos_modbus` | Modbus-RTU FC04 ab Register 0x1000 | U/I, Kapazitäten, SoC, SoH, Zyklen, Zellen, Temperaturen |

## Tests

```sh
make -C tests/host                                   # Protokollkern auf dem Host, kein Zephyr nötig
west twister -T batmon/tests -p native_sim --inline-logs   # dieselben Fälle als ztest
```

Die Testvektoren liegen in `tests/common/test_cases.c` und werden von beiden
Läufern benutzt. CI (`.github/workflows/build.yml`) baut die Firmware im
Zephyr-CI-Container, führt beide Testläufe aus und legt `zephyr.bin/.hex/.elf`
als Artefakt ab.

## Projektstruktur

```
west.yml                 West-Manifest (Zephyr v4.4.2, hal_stm32, cmsis, lvgl)
CMakeLists.txt, Kconfig, prj.conf, VERSION
boards/stm32h573i_dk.overlay   FDCAN2 auf PB5/PB6, RS485 auf USART3/PG15, Pin-Konflikte
src/main.c               Init, LED, Protokoll-Tick
src/can_rx.c             CAN-Empfang (Queue + Thread), Listen-only, Bench-Heartbeat
src/rs485.c              RS485: UART-Empfang, DE-Umschaltung, Abfrage- und Mithör-Schleife
src/net_mqtt.c           DHCP, MQTT-Client, HA-Discovery, ThingsBoard
src/ui.c                 LVGL-Statusbildschirm
src/shell_cmds.c         Shell-Kommandos `batmon ...`
src/proto/bm_proto.[ch]  Dekoder, ID-Statistik, Flavor-Erkennung (plain C)
src/proto/bm_json.c      Telemetrie-JSON (verschachtelt / flach)
src/proto/bm_ydt.[ch]    YD/T-1363-ASCII-Frames, Modbus-CRC, Hex-Helfer
src/proto/bm_serial.[ch] RS485-Protokollerkennung, Abfragezyklus, Decoder (plain C)
dts/bindings/            Devicetree-Binding für den RS485-Knoten
tests/common, tests/host, tests/proto   Testfälle, Host-Läufer, ztest
docs/protocols.md        Protokollbeschreibung
```

## Betriebshinweise

* Am Produktivbus immer Listen-only lassen. Ein zweiter "Wechselrichter" auf dem
  Bus (Heartbeat) kann das BMS verwirren.
* Bench-Modus: `CONFIG_BATMON_CAN_LISTEN_ONLY=n` und `CONFIG_BATMON_BENCH_HEARTBEAT=y`
  (oder zur Laufzeit `batmon hb on`). Die Batterie sieht dann einen
  Wechselrichter und sendet dauerhaft, der Schütz bleibt geschlossen.
* Kommt gar nichts an: Bitrate (500 kbit/s), CAN-H/CAN-L vertauscht, GND fehlt,
  Protokollwahl am Speicher (DIP-Schalter/App: "Pylontech"/"PYLON" statt
  RS485-Modus), Transceiver-Pegel (3,3 V).
* RS485 ohne Antwort: A/B tauschen, GND prüfen, DIP-Schalter für Baudrate und
  Adresse, am Speicher den RS485-Port "Inverter/RS485" (nicht den Link-Port zum
  nächsten Modul) benutzen. `batmon rs485 send ~2002464F0000FDA2 115200`
  schickt eine Versionsabfrage an Adresse 2 und zeigt die Rohantwort.
