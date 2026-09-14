# Table Football Feedback System ⚽

Hardware and bring-up work for a smart foosball table built for the UbiLab university course. The intended system identifies players with PN532 RFID readers, receives ball-tracker data, and shows match information on an EA DOGL128 display. A live Raspberry Pi dashboard, MQTT messaging, and match history are part of the broader project described in the original project notes, but their application code is **not in this repository yet**.

## What is in this repository

| File | Purpose |
|---|---|
| [`ubi_lab.kicad_sch`](ubi_lab.kicad_sch) | KiCad schematic for one side's Pico, RFID reader, display, and I2C connection |
| [`ubi_lab.kicad_pcb`](ubi_lab.kicad_pcb) | Matching two-layer PCB layout |
| [`ubilab.ino`](ubilab.ino) | Arduino PN532 electrical diagnostic for a separate SPI0 test wiring |
| [`spi1_diagnostic/spi1_diagnostic.ino`](spi1_diagnostic/spi1_diagnostic.ino) | Incomplete scratch sketch; it is not a working SPI1 test |
| [`third_party/Elechouse_PN532_Pico.zip`](third_party/Elechouse_PN532_Pico.zip) | Locally packaged PN532 Arduino library |
| [`enclosure_v2.scad`](enclosure_v2.scad), [`SCHEMATIC_MODEL_README.md`](SCHEMATIC_MODEL_README.md) | Parametric enclosure and its dimensions, print files, and fit notes |

The KiCad files are a **single-side controller design**. The full two-sided table would need the corresponding hardware for each side, a ball-tracker input, and firmware that implements the complete game behavior.

## Planned system

The project notes describe one controller per side of the table. Each side would read local RFID cards, receive ball data over a shared I2C connection, and show match feedback on its LCD. A Raspberry Pi 4 would host its own WiFi network, an MQTT broker, and a web dashboard with player and match history. These are design goals here; the uploaded files provide the controller schematic and PCB, not that finished software stack.

## KiCad controller board

Open the schematic and PCB together in **KiCad 10**. The PCB file specifies a two-layer, 1.6 mm board with a **76 × 68 mm** outline. It contains a socket footprint for a Pico-family board on the back, an 8-pin PN532 connector (`J1`), a 40-pin EA DOGL128L-6 display connector (`J2`), and a 4-pin I2C connector (`J3`, labeled `I2C_SIMULATOR`). `R1` and `R2` are 4.7 kΩ I2C pull-ups. The schematic also includes the display's charge-pump capacitors (eight 1 µF and one 4.7 µF).

The uploaded schematic and PCB use a `RaspberryPi_Pico_W` symbol/value and a Pico socket footprint whose description also mentions Pico 2 W support. The custom `ubi_lab:RaspberryPi_Pico_Socket_THT` footprint is embedded in the PCB, but its separate library was not supplied; KiCad may need that library to update the PCB from the schematic. Confirm the exact Pico variant, header orientation, display module, and connector mating before assembly.

### Pico signal map in the uploaded design

| Connection | Pico GPIO | Physical pin |
|---|---:|---:|
| I2C SDA / SCL | GP4 / GP5 | 6 / 7 |
| PN532 SCK / MOSI / MISO / CS | GP10 / GP11 / GP12 / GP13 | 14 / 15 / 16 / 17 |
| PN532 reset | GP15 | 20 |
| Display CS / SCK / MOSI / A0 / reset | GP17 / GP18 / GP19 / GP20 / GP21 | 22 / 24 / 25 / 26 / 27 |

`J1` supplies the PN532 connector from the board's **3.3 V** rail. `J3` carries VBUS, GND, SDA, and SCL on pins 1–4. Check the connected modules' voltage requirements and pin-1 orientation against the schematic before plugging them in. The PN532 module must be set to SPI mode; on the V3 module used for the diagnostic, switch 1 is **OFF** and switch 2 is **ON**. Change switches only with USB power disconnected.

Before sending the PCB to fabrication, run KiCad's electrical-rules and design-rules checks and reconcile schematic/PCB annotation. The supplied files contain capacitor reference inconsistencies (for example, a PCB reference such as `1µF7` where the schematic property uses a `C` reference). The enclosure's own README also lists physical measurements that still need confirmation against the assembled electronics.

## PN532 diagnostic currently included

[`ubilab.ino`](ubilab.ino) is a known-good **separate breadboard test** for the PN532. It uses bit-banged legacy SPI0 pins and powers the module from VBUS. **Its wiring does not match the KiCad controller board.** Use firmware configured for the PCB's GP10–GP15 signals when testing an assembled PCB.

For the breadboard diagnostic only, connect the PN532 as follows (Pico USB connector at the top, component side facing you):

| PN532 pin | Pico connection | Physical pin |
|---|---|---:|
| VCC | VBUS | 40 |
| GND | GND | 38 |
| SCK | GP2 | 4 |
| MOSI | GP3 | 5 |
| MISO | GP4 | 6 |
| SS | GP5 | 7 |

Leave PN532 `IRQ` and `RSTO` disconnected for this diagnostic. Disconnect USB before moving wires; do not leave GPIO signals connected to an unpowered PN532 module.

1. Install [`third_party/Elechouse_PN532_Pico.zip`](third_party/Elechouse_PN532_Pico.zip) in Arduino IDE with **Sketch → Include Library → Add .ZIP Library...**.
2. Select **Raspberry Pi Pico 2 W**, open `ubilab.ino`, and upload it.
3. Open Serial Monitor at **115200 baud**. The sketch reports the PN532 status, ACK, and firmware response, ending with `SUCCESS: PN532 is responding through legacy SPI0.` when the electrical link works.

This diagnostic confirms communication with the PN532; it does not implement player registration, scoring, display output, or the planned dashboard.

## Enclosure

The version-2 enclosure source and exported parts are in this repository. See [`SCHEMATIC_MODEL_README.md`](SCHEMATIC_MODEL_README.md) for print orientation, allowances, and fit checks. The documented electronics envelope is **76.2 × 93 × 31.3 mm**, which is an assembly allowance rather than the PCB outline. Verify the real board, connectors, and display-window position with a fit print before printing a full enclosure.
