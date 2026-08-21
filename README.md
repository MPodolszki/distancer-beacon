# distancer-beacon

iBeacon-Firmware für das PHYTEC *distancer*-Board (nRF52832), gebaut mit Zephyr.

Das Gerät sendet dauerhaft eine Apple-iBeacon-Advertisement aus. Es öffnet
selbst **keine** Tür: ein Empfänger (z. B. die Home-Assistant-iOS-App, die eine
iBeacon-Zone überwacht) erkennt den Beacon an UUID/Major/Minor und löst das
Schloss aus. Zusätzlich meldet die Firmware ihren Akkustand per
[BTHome v2](https://bthome.io/), das Home Assistant ohne Verbindung und ohne
Zusatzintegration als Batteriesensor erkennt.

Eckdaten:

| | |
|---|---|
| MCU | nRF52832 (Board `distancer`, rev 3) |
| Advertising-Intervall | 150–200 ms |
| Proximity UUID | `18ee1516-016b-4bec-ad96-bcb96d166e97` |
| Major / Minor | 1 / 1 |
| Sendeleistung | +4 dBm (Maximum des nRF52832) |
| Kalibrierter RSSI @ 1 m | `0xCA` = −54 dBm (bei +4 dBm Sendeleistung) |
| Batteriemeldung | BTHome v2, Messung alle 60 s, Sendung bei Änderung (Keep-alive 5 min) |
| Stromversorgung | NiMH-Pack, Ladung über LTC4060 (Micro-USB) |

---

## Bauen — Schritt für Schritt

Wer noch nie Zephyr gebaut hat, arbeitet diese Abschnitte einfach von oben nach
unten ab. Getestet unter Linux; unter macOS/Windows gelten dieselben Schritte,
nur die Paketnamen unterscheiden sich.

### 0. Was man dafür braucht

* Linux (getestet: Manjaro), Python ≥ 3.10, Git
* das **distancer-Board** samt DAPLink-Debug-Probe (on-board, meldet sich als
  `0d28:0204 NXP ARM mbed`)
* eine Internetverbindung für den ersten `west update` (lädt Zephyr und seine
  Module, ein paar hundert MB)

Mehr braucht es nicht — die Board-Definition liegt in diesem Repository, es wird
kein weiteres Repo benötigt.

### 1. Zephyr-Toolchain installieren

Einmalig, nach der offiziellen Anleitung:
<https://docs.zephyrproject.org/latest/develop/getting_started/index.html>

Kurzfassung — Systempakete installieren, dann ein Python-venv mit `west` und
das Zephyr-SDK:

```bash
python3 -m venv ~/zephyrproject/.venv
source ~/zephyrproject/.venv/bin/activate
pip install west pyocd
```

Das Zephyr-SDK (Cross-Compiler für ARM) installiert man wie in der Anleitung
beschrieben mit `west sdk install` bzw. dem SDK-Bundle. Ohne SDK bricht der
Build mit „toolchain not found" ab.

**Wichtig:** Vor *jedem* Build das venv aktivieren:

```bash
source ~/zephyrproject/.venv/bin/activate
```

### 2. Workspace anlegen

Diese Firmware ist eine *Zephyr-Workspace-Anwendung*: sie bringt Zephyr nicht
selbst mit, sondern lebt neben einem von west verwalteten Zephyr-Baum. Dieses
Repository ist dabei sein eigenes west-Manifest — `west init -m …` legt den
Workspace an, `west update` holt Zephyr in der getesteten Revision dazu:

```bash
west init -m https://github.com/MPodolszki/distancer-beacon.git ~/distancer-ws
cd ~/distancer-ws
west update                       # lädt Zephyr + Module (dauert ein paar Minuten)
west config build.sysbuild true   # sysbuild aktivieren (so wurde die Firmware getestet)
```

Danach sieht der Workspace so aus:

```
distancer-ws/
├── .west/                # west-Konfiguration
├── distancer-beacon/     # ← dieses Repository (Manifest-Repo)
├── zephyr/               # von west geholt
└── modules/              # von west geholt
```

Die Board-Definition `distancer` liegt in diesem Repository unter
[`boards/phytec/distancer/`](boards/phytec/distancer). Über
[`zephyr/module.yml`](zephyr/module.yml) meldet sich das Repo als Zephyr-Modul
mit `board_root: .` an — deshalb findet `west build -b distancer` das Board
automatisch, ohne dass man `-DBOARD_ROOT=…` angeben muss.

Damit das auch dann funktioniert, wenn das Repo *nicht* als Zephyr-Modul
registriert ist — also bei einem einfachen `git clone` in einen bereits
bestehenden west-Workspace —, setzen zusätzlich
[`CMakeLists.txt`](CMakeLists.txt) und
[`sysbuild/CMakeLists.txt`](sysbuild/CMakeLists.txt) `BOARD_ROOT` auf dieses
Verzeichnis. Beide Stellen sind nötig: ohne sysbuild ist `CMakeLists.txt` der
Einstiegspunkt, mit `--sysbuild` löst sysbuild das Board in einem eigenen
CMake-Scope auf, bevor es überhaupt in die Applikation absteigt.

### 3. Bauen

```bash
source ~/zephyrproject/.venv/bin/activate
cd ~/distancer-ws/distancer-beacon
west build -b distancer .
```

Ergebnis: `build/distancer-beacon/zephyr/zephyr.hex`.

Nach Konfigurationsänderungen (`prj.conf`, Overlay) sicherheitshalber neu
aufsetzen:

```bash
west build -b distancer . --pristine
```

### 4. Flashen

Der Debug-Probe auf dem Board ist ein DAPLink/CMSIS-DAP, deshalb **immer**
pyocd angeben — der Default-Runner ist J-Link und schlägt mit
`required program JLinkExe not found` fehl:

```bash
west flash --runner pyocd
```

Oder an west vorbei:

```bash
pyocd flash -e sector -t nrf52832 build/distancer-beacon/zephyr/zephyr.hex
```

Vorher prüfen, ob der Probe überhaupt da ist:

```bash
pyocd list                      # erwartet: ARM DAPLink CMSIS-DAP
pyocd cmd -t nrf52832 -c reg    # "Core is not halted" = Verbindung steht
```

### Wenn das Flashen fehlschlägt

`SWD/JTAG communication failure (No ACK)` heißt fast immer: der Probe wird
erkannt, **der Target-MCU hat aber keinen Strom oder schläft**. An den
Clock-/Connect-Flags zu drehen hilft nicht. Stattdessen:

1. Board einschalten, Akku laden bzw. Ladekabel anstecken.
2. Das Gerät fährt per Power-Button in System OFF herunter — vorher mit dem
   Button aufwecken oder das Ladegerät anstecken.
3. USB-Kabel/Port wechseln.
4. Letzter Ausweg: `pyocd erase --chip -t nrf52832` (löscht auch die
   APPROTECT-Sperre, danach ist der Flash leer).

Der DAPLink meldet sich zusätzlich als USB-Massenspeicher — dessen
Volume-Name (`reel-board`) taugt **nicht** zur Board-Identifikation, PHYTEC
nutzt dieselbe DAPLink-Kennung über mehrere Boards hinweg.

---

## Bedienung

**Tasten**

| Taste | Funktion |
|---|---|
| Ein/Aus (`P0.09`) | 2 s halten und loslassen → System OFF. Wieder aufwecken durch erneuten Druck oder Ladegerät anstecken |
| Mute (`P0.02`) | schaltet die iBeacon-Aussendung ab; das Gerät bleibt an und meldet weiter den Akkustand — die Tür geht dann nicht mehr automatisch auf |
| Ack (`P0.27`), User (`P0.10`) | derzeit ohne Funktion |

**LEDs** (beide RGB-Einheiten leuchten immer gleich)

| Anzeige | Bedeutung |
|---|---|
| grün, dauerhaft | lädt (Ladegerät steckt) |
| grün, kurzer Blitz 1×/s | läuft, sendet iBeacon (Normalbetrieb) |
| rot, kurzer Blitz 1×/s | Mute — läuft, sendet aber keinen iBeacon |
| orange, kurzer Blitz 3×/s | Akku leer |
| dunkel | ausgeschaltet |

Die Anzeige-Priorität ist: Laden → Aus → Akku leer → Mute → Normalbetrieb.

---

## Anpassen

Alle relevanten Werte stehen oben in [`src/main.c`](src/main.c):

* `IBEACON_UUID`, `IBEACON_MAJOR`, `IBEACON_MINOR` — müssen **exakt** zur
  Zonen-Konfiguration des Empfängers passen. Ein Scanner matcht auf UUID *und*
  Major *und* Minor; passt eines nicht, sieht es aus wie gar kein Beacon.
  (`1122`/`4455` sind die Defaults aus dem Zephyr-Sample — tauchen die hier
  wieder auf, hat jemand die echte Identität überschrieben.)
* `IBEACON_RSSI` — gemessene Signalstärke in 1 m Entfernung. Muss der
  Sendeleistung folgen (`CONFIG_BT_CTLR_TX_PWR_*` in `prj.conf`), sonst
  schätzen Empfänger die Entfernung falsch — der Wert wird beim Empfänger vom
  gemessenen RSSI abgezogen.

### Sendeleistung / Reichweite

Die Sendeleistung steht in `prj.conf` auf `CONFIG_BT_CTLR_TX_PWR_PLUS_4=y`.
+4 dBm ist die Obergrenze des nRF52832 — mehr kann der SoC nicht, dafür
bräuchte die Platine ein Front-End-Modul (PA). Wer die Einstellung anzweifelt,
prüft sie am Image statt per RSSI-Messung an einem einzelnen Punkt:

```
arm-zephyr-eabi-objdump -d build/distancer-beacon/zephyr/zephyr.elf \
    | grep -B1 'bl.*<radio_tx_power_set>'
```

Erwartet wird `movs r0, #4`; `radio_tx_power_set()` schreibt daraufhin `0x04`
(`Pos4dBm`) nach `RADIO->TXPOWER`. Ein Debug-Build gibt die Leistung zusätzlich
beim Start auf der Konsole aus (`Tx 4 dBm`).

Was ohne Hardware-Änderung sonst noch Reichweite bringt, ist nicht die
Sendeleistung, sondern die Trefferwahrscheinlichkeit: ein kürzeres
Advertising-Intervall (`ADV_INT_MIN`/`ADV_INT_MAX`) und ein kürzerer
BTHome-Burst (`BTHOME_BURST_MS`), während dessen der iBeacon-Frame gar nicht
gesendet wird. Beides kostet Akkulaufzeit.
* `ADV_INT_MIN` / `ADV_INT_MAX` — Advertising-Intervall, der dominierende
  Stromverbraucher. Kürzer = die Tür reagiert schneller, aber der Akku hält
  kürzer.
* `BATTERY_INTERVAL`, `BTHOME_BURST_MS` — während des BTHome-Bursts ersetzt der
  Batterie-Frame den iBeacon-Frame, die Türautomatik ist so lange blind.

Die Akku-Kennlinie (NiMH) steckt in [`src/battery.c`](src/battery.c).

Der Produktionsbuild hat UART/Logging abgeschaltet
([`prj.conf`](prj.conf)). Für Debug-Ausgaben zusätzlich
[`debug.conf`](debug.conf) einbinden:

```bash
west build -b distancer . -- -Ddistancer-beacon_EXTRA_CONF_FILE=debug.conf
```

Das Overlay [`boards/distancer.overlay`](boards/distancer.overlay) korrigiert
den Batterie-Spannungsteiler: auf der rev-3-Hardware ist der im Board-Devicetree
beschriebene Teiler nicht bestückt, die ADC-Leitung hängt direkt an der
Batterie.

---

## Lizenz

Apache-2.0. Basiert auf dem Zephyr-iBeacon-Sample
(© 2018 Henrik Brix Andersen), Erweiterungen © 2026 PHYTEC Messtechnik GmbH.
