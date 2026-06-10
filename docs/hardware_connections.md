# Table Football Feedback System — Hardware Connections

## Components

- Raspberry Pi Pico 2W (main board)
- Raspberry Pi Pico 2W (I2C simulator / ball tracker)
- PN532 RFID Reader
- EA DOGL128L-6 Display (128x64 reflective LCD)

---

## 1. PN532 RFID Reader

**DIP Switch Setting (SPI mode):**
```
SEL0 = OFF  (switch 1 down)
SEL1 = ON   (switch 2 up)
```

| PN532 Pin | Pico 2W GPIO | Pico 2W Physical Pin |
|-----------|-------------|----------------------|
| VCC       | 3.3V        | Pin 36               |
| GND       | GND         | Pin 38               |
| SCK       | GP10        | Pin 14               |
| MOSI      | GP11        | Pin 15               |
| MISO      | GP12        | Pin 16               |
| NSS / CS  | GP13        | Pin 17               |
| RSTO      | GP15        | Pin 20               |
| IRQ       | —           | Not connected        |

---

## 2. EA DOGL128L-6 Display

### Signal Pins

| Display Pin | Signal   | Pico 2W GPIO | Pico 2W Physical Pin |
|-------------|----------|-------------|----------------------|
| Pin 36      | SI (MOSI)| GP19        | Pin 25               |
| Pin 37      | SCL (SCK)| GP18        | Pin 24               |
| Pin 38      | A0       | GP20        | Pin 26               |
| Pin 39      | RST      | GP21        | Pin 27               |
| Pin 40      | CS1B     | GP17        | Pin 22               |

### Power Pins

| Display Pin | Signal | Connect To |
|-------------|--------|------------|
| Pin 35      | VDD    | 3.3V       |
| Pin 34      | VDD2   | 3.3V       |
| Pin 26      | VSS    | GND        |
| Pin 33      | VSS    | GND        |

### Capacitors (required for internal charge pump)

| Connection                      | Value        |
|---------------------------------|--------------|
| Pin 29 (CAP1P) ── Pin 30 (CAP1N)| 1µF ceramic  |
| Pin 28 (CAP2P) ── Pin 27 (CAP2N)| 1µF ceramic  |
| Pin 31 (CAP3P) ── GND           | 1µF ceramic  |
| Pin 32 (VOUT)  ── GND           | 4.7µF electrolytic (+ leg on pin 32) |
| Pin 21 (V0) ── Pin 22 (V1)      | 1µF ceramic  |
| Pin 22 (V1) ── Pin 23 (V2)      | 1µF ceramic  |
| Pin 23 (V2) ── Pin 24 (V3)      | 1µF ceramic  |
| Pin 24 (V3) ── Pin 25 (V4)      | 1µF ceramic  |
| Pin 25 (V4) ── GND              | 1µF ceramic  |

> Total: 9 capacitors. All required. Missing any one will prevent the display from working.

---

## 3. I2C Bus (Main Pico ↔ Simulator Pico)

| Signal | Main Pico        | Simulator Pico   |
|--------|-----------------|-----------------|
| SDA    | GP4 (Pin 6)     | GP4 (Pin 6)     |
| SCL    | GP5 (Pin 7)     | GP5 (Pin 7)     |
| GND    | GND             | GND             |

### Pull-up Resistors (required, place on either board)

```
3.3V ──[4.7kΩ]── SDA line
3.3V ──[4.7kΩ]── SCL line
```

### Power — Simulator Pico powered from Main Pico

| Main Pico       | Simulator Pico  |
|----------------|----------------|
| VBUS (Pin 40)  | VSYS (Pin 39)  |
| GND            | GND            |

---

## 4. Complete GPIO Summary — Main Pico 2W

| GPIO | Physical Pin | Function         | Connected To         |
|------|-------------|------------------|----------------------|
| GP4  | Pin 6       | I2C0 SDA         | Simulator Pico GP4   |
| GP5  | Pin 7       | I2C0 SCL         | Simulator Pico GP5   |
| GP10 | Pin 14      | SPI1 SCK         | PN532 SCK            |
| GP11 | Pin 15      | SPI1 MOSI        | PN532 MOSI           |
| GP12 | Pin 16      | SPI1 MISO        | PN532 MISO           |
| GP13 | Pin 17      | GPIO out (CS)    | PN532 NSS/CS         |
| GP15 | Pin 20      | GPIO out (RST)   | PN532 RSTO           |
| GP17 | Pin 22      | GPIO out (CS)    | Display CS (pin 40)  |
| GP18 | Pin 24      | GPIO out (SCK)   | Display SCL (pin 37) |
| GP19 | Pin 25      | GPIO out (MOSI)  | Display SI (pin 36)  |
| GP20 | Pin 26      | GPIO out (A0)    | Display A0 (pin 38)  |
| GP21 | Pin 27      | GPIO out (RST)   | Display RST (pin 39) |

---

## 5. Complete GPIO Summary — Simulator Pico 2W

| GPIO | Physical Pin | Function   | Connected To       |
|------|-------------|------------|--------------------|
| GP4  | Pin 6       | I2C0 SDA   | Main Pico GP4      |
| GP5  | Pin 7       | I2C0 SCL   | Main Pico GP5      |

---

## 6. Power Rail Summary

| Rail         | Source              | Powers                                  |
|--------------|--------------------|-----------------------------------------|
| 5V (VBUS)    | PC USB             | Main Pico, then to Simulator Pico VSYS  |
| 3.3V         | Main Pico regulator| PN532 VCC, Display VDD/VDD2             |
| GND          | Common             | All components                          |

> Total estimated current: ~175mA. Well within the 600mA limit of the Pico 2W regulator.

---

## 7. Software Configuration

| Parameter         | Value  |
|-------------------|--------|
| SPI1 speed        | 1 MHz  |
| I2C0 speed        | 9600 baud |
| I2C slave address | 0x42   |
| Display contrast  | 0x13   |
| Display type      | SPI (bit-bang, no hardware SPI) |
| RFID interface    | SPI1 (hardware SPI)             |

---

## Notes

- The EA DOGL128L-6 is the **reflective** variant — no backlight is possible or needed.
- The display uses **bit-bang SPI** (not hardware SPI) because hardware SPI timing caused issues with this clone variant.
- The PN532 **IRQ pin is not used** — the driver polls for card presence.
- The I2C slave IRQ is temporarily disabled during RFID transactions to prevent timing interference.
- Both Picos share a common GND — this is required for I2C to work correctly.
