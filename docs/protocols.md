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
