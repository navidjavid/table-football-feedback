# Table Football Feedback System ⚽

A smart foosball table feedback system built for the UbiLab university course. This project uses a Raspberry Pi Pico 2 W as the main controller to process real-time game data, identify players via RFID, and display live match statistics.

## 🚀 Features

* **Player Identification:** Players scan their RFID tags to log into a match.
* **Live Score Tracking:** Real-time score updates displayed on the screen.
* **Shot Analytics:** Calculates and displays the fastest shot speed.
* **Match History:** Displays previous scores and game statistics.
* **Distributed Architecture:** Receives real-time ball tracking data over I2C from a secondary vision/tracking board.

## 🛠️ Hardware Stack

* **Main Microcontroller:** Raspberry Pi Pico 2 W (RP2350)
* **Display:** EA DOGL128-6 (128x64 Graphic LCD)
* **RFID Reader:** MFRC522
* **External Tracker:** Custom board sending data via I2C

## 🔌 Pin Configuration

*(Note: Update these pins based on your actual wiring!)*

| Component      | Pin Function | Pico 2 W Pin (GPIO) |
| :---           | :---         | :---                |
| **MFRC522** | SDA (CS)     | GPIO 5              |
|                | SCK          | GPIO 6              |
|                | MOSI         | GPIO 7              |
|                | MISO         | GPIO 4              |
| **EA DOGL128** | CS           | GPIO 13             |
|                | SCK          | GPIO 14             |
|                | MOSI         | GPIO 15             |
|                | A0 (Data/Cmd)| GPIO 12             |
| **I2C Tracker**| SDA          | GPIO 16             |
|                | SCL          | GPIO 17             |

## 🏗️ Getting Started

### Prerequisites
* [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) configured on your machine.
* CMake and Make.

### Building the Project
1. Clone the repository:
   ```bash
   git clone [https://github.com/yourusername/table-football-feedback.git](https://github.com/yourusername/table-football-feedback.git)
   cd table-football-feedback