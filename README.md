# batmon – Batterie-CAN-Monitor für das STM32H573I-DK

batmon ist eine Zephyr-RTOS-Anwendung, die am CAN-Bus zwischen Batteriespeicher
(BMS) und Wechselrichter mithört, das Protokoll und den Hersteller-"Flavor"
erkennt (Pylontech-Original, PACE/FM-Solar, Seplos, JK, Felicity, BYD,
SMA-Dialekt, Victron-Erweiterung, Pylontech-HV, Growatt-/GoodWe-Marker), den
Batteriezustand dekodiert, ihn auf dem Display des Discovery-Kits anzeigt und
per MQTT über die Ethernet-Buchse weiterreicht – generisch (mit Home-Assistant-
Discovery) oder im ThingsBoard-Geräteformat.

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
```

Dazu die Zephyr-Shells `can`, `net` (z. B. `net iface`, `net ping`) und
`device list`.

## Display

Kopfzeile mit Device-ID und CAN-Link, großer SoC mit Balken, Spannung/Strom/
Temperatur, Lade- und Entladegrenzen, Freigabeflags (bei aktiver Protection rot
"SCHUTZ AKTIV"), erkannter Protokoll-Flavor mit Hersteller und Konfidenz, unten
IP-Adresse, MQTT-Status und Rahmenzähler. Die blaue LED leuchtet dauerhaft,
solange Batterierahmen eintreffen, und blinkt sonst.

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
boards/stm32h573i_dk.overlay   FDCAN2 auf PB5/PB6, Bitrate, Pin-Konflikte
src/main.c               Init, LED, Protokoll-Tick
src/can_rx.c             CAN-Empfang (Queue + Thread), Listen-only, Bench-Heartbeat
src/net_mqtt.c           DHCP, MQTT-Client, HA-Discovery, ThingsBoard
src/ui.c                 LVGL-Statusbildschirm
src/shell_cmds.c         Shell-Kommandos `batmon ...`
src/proto/bm_proto.[ch]  Dekoder, ID-Statistik, Flavor-Erkennung (plain C)
src/proto/bm_json.c      Telemetrie-JSON (verschachtelt / flach)
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
