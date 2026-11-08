# MIH Classroom Timer

We built this project in our third-year college Microprocessor class to help students see the current time and how much class time is left. A large four-digit seven-segment LED display makes the timer easy to read from across the classroom.

## What it does

- Shows a 24-hour clock, a countdown, or both. In dual mode, the controller LCD shows both at once and the large display alternates between them every five seconds.
- Lets you set the clock manually and set, start, or pause the countdown with a keypad. The clock can also sync over Wi-Fi using NTP (UTC+8).
- Sounds a buzzer at countdown milestones and when time runs out. Press any key to dismiss the time-up alert.

## Requirements

- **ESP-1 (controller)** runs `ESP1_Brain/ESP1_Brain.ino` and uses a 4×4 keypad and a 16×2 I²C LCD (address `0x27`).
- **ESP-2 (display driver)** runs `ESP2_Driver/ESP2_Driver.ino` and uses a large four-digit seven-segment LED display, compatible shift-register driver circuitry, and a buzzer.
- **Software** requires Arduino IDE with ESP32 board support and the `Keypad` and `LiquidCrystal_I2C` libraries. The sketches use [legacy ESP-NOW callbacks](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/network/esp_now.html).
- **Wireless setup** requires two dual-core ESP32 boards. The controller's configured ESP-NOW peer MAC must match the display board's Wi-Fi MAC. Internet access is needed for automatic clock sync.

## Quick controls

Press **D** to switch views. In Clock or Timer view, press **C** to enter hours, minutes, and seconds. Press **C** after each field and **A** to confirm a manual clock change. In Timer view, press **A** to start or pause the countdown.

**Build note** — The supplied `ESP1_Brain.ino` contains two copies of the sketch. A build copy needs a single set of definitions before it can compile.
