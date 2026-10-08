# Batterie-CAN-Protokolle, die batmon versteht

batmon hört den CAN-Bus zwischen Batterie (BMS) und Wechselrichter mit, dekodiert
die bekannten Rahmen und ordnet den Verkehr einer Protokollfamilie und, soweit
möglich, einem Hersteller-"Flavor" zu. Alle Dekoder liegen in `src/proto/` und sind
reines C ohne Zephyr-Abhängigkeit (Host-Tests: `make -C tests/host`).

## 1. Pylontech-LV-Protokoll (Basis fast aller 48-V-Speicher)

Physik: 500 kbit/s, 11-Bit-Identifier, Nutzdaten **Little-Endian**. Pylontech
RJ45 "CAN": Pin 4 = CAN-H, Pin 5 = CAN-L (PACE/FM-Solar, Seplos, Dyness u. a.
übernehmen diese Belegung, Felicity/JK bitte im Handbuch prüfen).

| ID    | Bytes | Inhalt | Skalierung |
|-------|-------|--------|------------|
| 0x351 | 0–1 u16 | Ladespannungsgrenze CVL | 0,1 V |
|       | 2–3 s16 | Ladestromgrenze CCL | 0,1 A |
|       | 4–5 s16 | Entladestromgrenze DCL | 0,1 A |
|       | 6–7 u16 | Entladeschlussspannung DVL (nur bei DLC 8) | 0,1 V |
| 0x355 | 0–1 u16 | SoC | % |
|       | 2–3 u16 | SoH | % |
|       | 4–5 u16 | SoC hochauflösend (optional, nicht ausgewertet) | 0,01 % |
| 0x356 | 0–1 s16 | Pack-Spannung | 0,01 V |
|       | 2–3 s16 | Pack-Strom, + = Laden | 0,1 A |
|       | 4–5 s16 | Temperatur | 0,1 °C |
| 0x359 | 0–1 | Protection-Bits (siehe unten) | |
|       | 2–3 | Alarm-Bits (siehe unten) | |
|       | 4 | Anzahl Module | |
|       | 5–6 | ASCII `P` `N` – Pylontech-Marker | |
| 0x35C | 0 | Bit 7 Laden erlaubt, Bit 6 Entladen erlaubt, Bit 5 Zwangsladung I, Bit 4 Zwangsladung II, Bit 3 Vollladung anfordern | |
| 0x35E | 0–7 | Herstellername ASCII (`PYLON   `, `PACE`, `SEPLOS`, ...) | |

Protection-Bits (0x359 Byte 0/1) und Alarm-Bits (Byte 2/3), wie batmon sie benennt:

| Byte | Bit | Protection (Byte 0/1) | Alarm (Byte 2/3) |
|------|-----|-----------------------|------------------|
| 0 / 2 | 1 | `cell_overvoltage` | `cell_high_voltage` |
| 0 / 2 | 2 | `cell_undervoltage` | `cell_low_voltage` |
| 0 / 2 | 3 | `cell_overtemp` | `cell_high_temp` |
| 0 / 2 | 4 | `cell_undertemp` | `cell_low_temp` |
| 0 / 2 | 7 | `discharge_overcurrent` | `discharge_high_current` |
| 1 / 3 | 0 | `charge_overcurrent` | `charge_high_current` |
| 1 / 3 | 3 | `system_error` | `internal_comm_fail` |

Wechselrichter-Seite (wird mitgezählt, aber nicht als Batterieverkehr gewertet):

| ID    | Inhalt |
|-------|--------|
| 0x305 | Keep-alive/Heartbeat des Wechselrichters, 8 Byte (meist 0). Viele BMS-Firmwares (auch PACE) öffnen ohne Heartbeat nach einigen Sekunden den Schütz oder stellen das Senden ein. Im Bench-Modus sendet batmon diesen Rahmen selbst (`CONFIG_BATMON_BENCH_HEARTBEAT`, Shell `batmon hb on`). |
| 0x307 | Identifikation des Wechselrichters (Victron: `VIC`, SMA/Deye u. a. teils andere ASCII-Kennungen). |

## 2. SMA-Li-Ion-Dialekt (BYD, LG, Sunny Island / Sunny Boy Storage)

Gleiche 0x351/0x355/0x356-Basis, zusätzlich:

| ID    | Inhalt |
|-------|--------|
| 0x35A | 8 Byte Alarm-/Warnungs-Bitpaare nach SMA-Spezifikation (batmon speichert die Rohbytes als `alarm35a`). |
| 0x35F | Byte 0–1 Batterietyp/Chemie, 2–3 Hardwareversion, 4–5 Kapazität in Ah, 6–7 Softwareversion. |

Erkennung: 0x35A oder 0x35F vorhanden, aber weder 0x35C noch 0x359 → `sma_lv`.

## 3. Victron-CAN-BMS-Erweiterung (BYD, Pylontech-Firmware ab 2021, Seplos, ...)

| ID    | Inhalt |
|-------|--------|
| 0x370 / 0x371 | Batteriename ASCII, 16 Zeichen (`BYD Battery-Box ...`) |
| 0x372 | u16: Module online, Module blockieren Laden, Module blockieren Entladen, Module offline |
| 0x373 | u16: min. Zellspannung mV, max. Zellspannung mV, min. Zelltemperatur K, max. Zelltemperatur K |
| 0x374 / 0x375 | ASCII-Name der Zelle mit min. / max. Spannung |
| 0x376 / 0x377 | ASCII-Name des Sensors mit min. / max. Temperatur |
| 0x378 | u32: geladene Energie, entladene Energie (0,1 kWh, roh gespeichert) |
| 0x379 | u16: installierte Kapazität in Ah |

Erkennung: 0x370–0x379 vorhanden → `victron_ext`.

## 4. Pylontech-HV-Protokoll (Force H1/H2, Powercube, SMA Sunny Boy Storage, GoodWe-HV ...)

29-Bit-Identifier, Nutzdaten **Big-Endian**, Offsets wie im Pylontech-HV-Protokoll.

| ID     | Inhalt |
|--------|--------|
| 0x4210 | u16 Spannung 0,1 V; u16 Strom 0,1 A (Offset 30000); u16 Temperatur 0,1 °C (Offset 1000); u8 SoC; u8 SoH |
| 0x4220 | u16 CVL 0,1 V; u16 DVL 0,1 V; u16 CCL 0,1 A (Offset 30000); u16 DCL 0,1 A (Offset 30000) |
| 0x4230 | u16 max. Zellspannung mV; u16 min. Zellspannung mV; u16 Nr. der Zelle max.; u16 Nr. der Zelle min. |
| 0x4240 | u16 max. Zelltemperatur 0,1 °C (Offset 1000); u16 min. Zelltemperatur; u16 Nr. max.; u16 Nr. min. |
| 0x4250 | Byte 0 Basisstatus, Byte 1–2 Zyklen, Byte 3 Fehler, Byte 4–5 Alarm, Byte 6–7 Protection |
| 0x4280 | Byte 0 Laden verboten (0xAA), Byte 1 Entladen verboten (0xAA) |
| 0x4290 | Fehlererweiterung |
| 0x4200 (vom WR) | Anfrage: Byte 0 = 0x00 Ensemble-Daten, 0x02 Systeminfo |
| 0x7310 / 0x7320 / 0x7330 / 0x7340 | Versionen, Ensemble-Info, Herstellername ASCII (erfasst, nicht dekodiert) |

Erkennung: Identifier im Bereich 0x4210–0x42FF → `pylon_hv`.

## 5. Weitere Marker

| Familie | Erkennung | Hinweis |
|---------|-----------|---------|
| `growatt_lv` | 0x311 und 0x313 vorhanden | Growatt-BMS-Protokoll (ARK/AXE LV); nur Erkennung, keine Dekodierung |
| `goodwe_lv` | 0x455 und 0x456 vorhanden | GoodWe-LV-Protokoll (Lynx, BYD im GoodWe-Modus); nur Erkennung |

## 6. Hersteller-Flavor aus 0x35E

Der Herstellerstring (Großschreibung, Präfix-Vergleich) wird auf einen Hersteller
abgebildet. Tabelle in `bm_vendor_from_string()`:

| Präfix | Hersteller | typische Geräte |
|--------|-----------|-----------------|
| `PYLON` | Pylontech | US2000/US3000/US5000, Force L1/L2 |
| `PACE` | PACE-BMS | FM-Solar / DelongTop 51,2 V, Gobel, SOK, viele No-Name-Racks |
| `SEPLOS` | Seplos | Mason, DIY 48 V |
| `JK`, `JIKONG` | JK-BMS | JK PB-Serie im Pylontech-Modus |
| `FELIC` | Felicity Solar | LPBF/LPBA 48 V |
| `BYD` | BYD | Battery-Box LVS/LVL/HVS/HVM |
| `SMA` | SMA | |
| `DYNESS`, `HUBBLE`, `EG4`, `DEYE`, `GROWATT`, `GOODWE`, `LUX` | wie benannt | |

Pylontech sendet im Original `PYLON` **plus** den `PN`-Marker in 0x359 und 0x351 mit
DLC 8. Emulationen (PACE, Seplos, JK ...), die sich als `PYLON` ausgeben, lassen
den Marker meist weg oder kürzen 0x359 auf 4 Byte, deshalb meldet batmon in diesem
Fall "Pylontech-kompatibel, Emulation".

## 7. Erkennungsheuristik und Konfidenz

Reihenfolge in `bm_detect_run()`:

1. keine Batterierahmen → `unknown`, "kein Batterieverkehr"
2. IDs 0x4210–0x42FF gesehen → `pylon_hv` (40 % + 8 % je bekanntem HV-Rahmen)
3. 0x351 + 0x355 + 0x356 gesehen → Basis 50 %, je +10 % für 0x359, 0x35C, 0x35E,
   `PN`-Marker und mehr als 20 Rahmen:
   * 0x370+ gesehen → `victron_ext`
   * 0x35A/0x35F ohne 0x35C/0x359 → `sma_lv`
   * sonst → `pylon_lv` mit Hersteller-Flavor
4. 0x311 + 0x313 → `growatt_lv` (70 %)
5. 0x455 + 0x456 → `goodwe_lv` (70 %)
6. sonst `unknown`

Die Hints (`proto.hints` im JSON, Bitmaske) halten die Einzelbeobachtungen fest:

| Bit | Name | Bedeutung |
|-----|------|-----------|
| 0 | `PN_MARKER` | `PN` in 0x359 |
| 1 | `351_DLC8` | 0x351 mit 8 Byte (DVL vorhanden) |
| 2 | `35F_PRESENT` | 0x35F gesehen |
| 3 | `35A_PRESENT` | 0x35A gesehen |
| 4 | `VICTRON_EXT` | 0x370–0x379 gesehen |
| 5 | `HV_IDS` | 0x42xx gesehen |
| 6 | `INV_HEARTBEAT` | 0x305 gesehen |
| 7 | `INV_IDENT` | 0x307 gesehen |
| 8 | `MULTI_MODULE` | Modulanzahl > 1 |
| 9 | `MFR_STRING` | druckbarer Herstellerstring |
| 10 | `35C_EXTRA` | 0x35C mit mehr als 2 Byte |
| 11 | `359_SHORT` | 0x359 kürzer als 7 Byte |

Link-Status: `link_up` fällt auf `false`, wenn 5 s lang kein Batterierahmen kommt.

## 8. RS485-Protokolle

Die serielle Seite wird aktiv abgefragt (batmon ist Master) oder im
Listen-only-Modus mitgehört. Dekoder und Zustandsautomat liegen in
`src/proto/bm_serial.c`, die Rahmenhelfer in `src/proto/bm_ydt.c`.

### 8.1 YD/T-1363-ASCII-Rahmen (Pylontech, PACE, Seplos V2)

```
~ VER ADR CID1 CID2 LENGTH INFO CHKSUM \r
```

* alle Felder als Hex-Zeichen; VER 20 (Pylontech, Seplos, ältere PACE) oder
  25 (PACE); ADR = Pack-Adresse (Pylontech 2 für das erste Modul, PACE 1,
  Seplos 0); CID1 46 = Batterie; CID2 = Kommando, in der Antwort der
  Rückgabecode (00 ok, 01 Versionsfehler, 02 Prüfsumme, 04 Kommando unbekannt,
  90 Adressfehler ...)
* LENGTH = LCHKSUM (4 Bit: Zweierkomplement der Nibble-Summe von LENID) +
  LENID (12 Bit: Anzahl INFO-Zeichen)
* CHKSUM = Zweierkomplement der Summe aller Zeichen von VER bis INFO

Beispiele: `~20024642E00202FD33` (Pylontech, Analogwerte Pack 2),
`~25014642E00201FD30` (PACE), `~20004642E00200FD37` (Seplos),
`~200246510000FDAC` (Herstellerinfo).

| CID2 | Inhalt der Antwort (INFO, Bytes big-endian) |
|------|---------------------------------------------|
| 0x42 Analogwerte | Flag, Pack, Zellenzahl N, N×u16 mV, Temperaturzahl M, M×u16 (0,1 K, −2731), dann je nach Hersteller: **Pylontech** s16 Strom 0,1 A, u16 Spannung mV, u16 Restkapazität mAh, u8 "user defined" (02/04), u16 Vollkapazität mAh, u16 Zyklen, bei 04 zusätzlich u24 Rest-/Vollkapazität mAh. **PACE** s16 Strom 0,01 A, u16 mV, u16 Rest 0,01 Ah, u8 03, u16 Voll 0,01 Ah, u16 Zyklen, u16 Nennkapazität 0,01 Ah. **Seplos** s16 Strom 0,01 A, u16 Spannung 0,01 V, u16 Rest 0,01 Ah, u8 0A, u16 Voll 0,01 Ah, u16 SoC 0,1 %, u16 Nennkapazität 0,01 Ah, u16 Zyklen, u16 SoH 0,1 %, u16 Port-Spannung 0,01 V |
| 0x44 Alarm/Status | Flag, Pack, N, N×u8 Zellstatus, M, M×u8 Temperaturstatus, danach herstellerspezifische Statusbytes (batmon speichert sie roh als `status_raw`) |
| 0x51 Herstellerinfo | 10 Byte Modell ASCII, 2 Byte Softwareversion, 20 Byte Hersteller ASCII |
| 0x92 Lade-/Entlademanagement (Pylontech) | Pack, u16 CVL mV, u16 DVL mV, s16 CCL 0,1 A, s16 DCL 0,1 A, u8 Status (Bit 7 Laden erlaubt, Bit 6 Entladen erlaubt, Bit 5/4 Zwangsladung, Bit 3 Vollladung) |
| 0x93 Seriennummer (Pylontech) | Pack, 16 Byte ASCII |
| 0x4F Protokollversion | keine INFO, VER der Antwort ist die Version |

Bei unbekanntem Hersteller (`generic_ascii`) wird das Layout am "user defined"-
Byte erkannt (0A Seplos, 03 PACE, 02/04 Pylontech) und sonst daran, ob die
Spannung in mV oder 0,01 V zur Summe der Zellspannungen passt.

### 8.2 JK-BMS RS485 (binär)

Anfrage `4E 57 00 13 00 00 00 00 06 03 00 00 00 00 00 00 68 00 00 01 29`
(115200 8N1). Antwort: `4E 57` LEN(2) Terminal(4) `06` Quelle(1) Typ(1), dann
Datensätze `ID Wert` mit fester Breite pro ID (0x79 Zellspannungen: Länge +
Tripel Nr/u16 mV; 0x80–0x82 Temperaturen °C, Werte > 100 sind negativ;
0x83 Spannung 0,01 V; 0x84 Strom 0,01 A, Bit 15 = Laden; 0x85 SoC; 0x87
Zyklen; 0x8B Alarmbits; 0x8C Statusbits; 0xAA Kapazität Ah; 0xB4 Geräte-ID;
0xB7 Firmware; 0xBA Hersteller), Record(4), `68`, Prüfsumme (4 Byte, 16-Bit-
Summe aller vorherigen Bytes). LEN zählt ab dem LEN-Feld bis zum Ende.
Alte Firmware kodiert den Strom als 10000 − Rohwert
(`CONFIG_BATMON_JK_CURRENT_LEGACY`).

### 8.3 Seplos V3 Modbus RTU

19200 8N1, Slave-Adresse 0 für das erste Pack (DIP-Schalter), Funktion 04:

| Register | Inhalt |
|----------|--------|
| 0x1000 … | Pack-Spannung 0,01 V, Strom 0,01 A (signed), Restkapazität 0,01 Ah, Gesamtkapazität 0,01 Ah, Entladesumme 0,1 Ah, SoC 0,1 %, SoH 0,1 %, Zyklen, mittlere Zellspannung mV, mittlere Zelltemperatur 0,1 K, max./min. Zellspannung, max./min. Zelltemperatur |
| 0x1100 … | 16 Zellspannungen mV, 4 Zelltemperaturen 0,1 K, 0x1118 MOSFET-, 0x1119 Umgebungstemperatur |

### 8.4 Erkennungsablauf

Ohne feste Vorgabe werden nacheinander probiert: Pylontech 115200/Adr 2, PACE
9600/Adr 1, Pylontech 9600, Seplos 9600/Adr 0, JK 115200, Seplos-V3-Modbus
19200/Adr 0, dann die Varianten mit vertauschten Baudraten und Adressen 0/1.
Jede gültige ASCII-Antwort (auch ein Fehlercode) zählt als gefundenes Gerät;
die Antwort auf 0x51 legt den Flavor fest, ihre VER die Protokollversion. Danach
läuft die Abfrage zyklisch (`CONFIG_BATMON_RS485_POLL_MS`): 0x42, 0x44 und bei
Pylontech 0x92 (einmalig 0x93). Kommandos, die mit Rückgabecode ≠ 0 beantwortet
werden, fallen aus dem Zyklus. Nach drei Fehlern in Folge gilt der Link als
unterbrochen, nach zehn beginnt die Erkennung von vorn.

Im Listen-only-Modus werden Anfragen (CID2 42/44/47/4F/51/92/93, INFO ≤ 2 Byte)
und Antworten (CID2 = Rückgabecode) unterschieden; die Antwort wird mit dem
Layout der zuletzt gesehenen Anfrage dekodiert. Baudraten werden rotiert, bis
gültige Rahmen erscheinen.
