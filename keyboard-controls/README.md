# Disclaimer
This README was generated with the assistance of AI but then reviewed and tested by me.

# ESP32 BLE Keyboard LED Controller

This project lets an ESP32 connect to a Bluetooth keyboard and use keyboard buttons to control WS2812B LEDs.

## Controls

* Left Arrow → previous color
* Right Arrow → next color
* Volume Up → brighter
* Volume Down → dimmer
* Mute → LEDs on/off

The code uses NimBLE-Arduino and FastLED.

---

# What You Need

## Hardware

* ESP32 board with Bluetooth
* WS2812B LED strip
* BLE keyboard
* USB data cable
* 5V power supply for the LEDs

The current code uses:

```cpp
#define LED_PIN 2
#define NUM_LEDS 144
```

So the LED data wire should be connected to GPIO 2 unless you change the code.

---

# Software

Install:

1. Arduino IDE 2
2. ESP32 board package by Espressif Systems
3. NimBLE-Arduino 2.x
4. FastLED

---

# 1. Install Arduino IDE

Download and install Arduino IDE 2.

Open it after installation.

---

# 2. Install ESP32 Support

In Arduino IDE go to:

**Tools → Board → Boards Manager**

Search:

```text
esp32
```

Install:

```text
esp32 by Espressif Systems
```

Then select your board under:

**Tools → Board → esp32**

For many generic ESP32 boards you can use:

```text
ESP32 Dev Module
```

---

# 3. Install the Libraries

Go to:

**Sketch → Include Library → Manage Libraries**

Search for and install:

```text
NimBLE-Arduino
```

Use NimBLE-Arduino **2.x**.

Then install:

```text
FastLED
```

---

# 4. Find Your Keyboard's BLE Address

The ESP32 code only connects to one specific keyboard.

The current address in the code is:

```cpp
static const NimBLEAddress ALLOWED_ADDR(
  std::string("fd:e5:18:ff:25:c8"),
  BLE_ADDR_RANDOM
);
```

You need to replace that address if your keyboard uses a different one.

## Simple BLE Scanner

Create a new Arduino sketch and upload this first:

```cpp
#include <Arduino.h>
#include <NimBLEDevice.h>

static NimBLEUUID HID_SERVICE((uint16_t)0x1812);

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {

    if (!device->isAdvertisingService(HID_SERVICE)) {
      return;
    }

    Serial.println("------------------");

    Serial.print("Name: ");
    Serial.println(device->getName().c_str());

    Serial.print("Address: ");
    Serial.println(
      device->getAddress().toString().c_str()
    );

    Serial.print("Address type: ");
    Serial.println(
      device->getAddress().getType()
    );
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);

  NimBLEDevice::init("");

  NimBLEScan* scan = NimBLEDevice::getScan();

  scan->setScanCallbacks(new ScanCB(), true);
  scan->setActiveScan(true);

  Serial.println("Scanning for BLE keyboards...");

  scan->start(0, false, true);
}

void loop() {
  delay(1000);
}
```

Open:

**Tools → Serial Monitor**

Set it to:

```text
115200
```

Put your keyboard into the Bluetooth slot you want to use with the ESP32.

You should see something like:

```text
Name: My Keyboard
Address: fd:e5:18:ff:25:c8
Address type: 1
```

The important part is the address:

```text
fd:e5:18:ff:25:c8
```

Copy it.

If multiple devices appear, turn your keyboard off and back on and watch which device disappears and returns.

---

# 5. Put the Address in the Main Code

Open the main LED controller code.

Find:

```cpp
static const NimBLEAddress ALLOWED_ADDR(
  std::string("fd:e5:18:ff:25:c8"),
  BLE_ADDR_RANDOM
);
```

Replace the address with the one from the scanner.

Example:

```cpp
static const NimBLEAddress ALLOWED_ADDR(
  std::string("aa:bb:cc:dd:ee:ff"),
  BLE_ADDR_RANDOM
);
```

Do not change this:

```cpp
0x1812
```

That is the standard BLE HID service used by devices like keyboards.

---

# 6. Create the Main Sketch

Create a new Arduino sketch.

Paste the full ESP32 LED controller code into it.

Save it as something like:

```text
ESP32_BLE_LED_Controller
```

---

# 7. Connect the ESP32

Connect the ESP32 to your computer using a USB **data** cable.

Go to:

**Tools → Port**

Select the port for your ESP32.

On Windows it may look like:

```text
COM3
```

---

# 8. Compile

Click:

**Verify**

If Arduino says:

```text
NimBLEDevice.h: No such file or directory
```

install NimBLE-Arduino.

If it says:

```text
FastLED.h: No such file or directory
```

install FastLED.

---

# 9. Upload

Click:

**Upload**

If it gets stuck at:

```text
Connecting...
```

hold the **BOOT** button on the ESP32 until uploading begins.

---

# 10. Open Serial Monitor

Open:

**Tools → Serial Monitor**

Set:

```text
115200 baud
```

The main code also uses 115200 baud.

You should eventually see something similar to:

```text
ESP32 HID LED Controller
Scanning for keyboard...
Found keyboard
BLE connected
BLE encrypted
Ready for keyboard input
```

---

# 11. Use the Keyboard

Once connected:

```text
Left Arrow     Previous color
Right Arrow    Next color
Volume Up      Increase brightness
Volume Down    Decrease brightness
Mute           LEDs on/off
```

If you have a tri-mode keyboard, dedicate one Bluetooth slot to the ESP32.

For example:

```text
Bluetooth 1 → Laptop
Bluetooth 2 → ESP32
Bluetooth 3 → Phone
```

---

# Quick Setup Checklist

* [ ] Install Arduino IDE
* [ ] Install ESP32 board package
* [ ] Install NimBLE-Arduino 2.x
* [ ] Install FastLED
* [ ] Upload the BLE scanner
* [ ] Find your keyboard BLE address
* [ ] Put the address into `ALLOWED_ADDR`
* [ ] Paste the main code into Arduino IDE
* [ ] Select your ESP32 board
* [ ] Select the correct port
* [ ] Upload
* [ ] Open Serial Monitor at 115200
* [ ] Switch keyboard to the ESP32 Bluetooth slot
* [ ] Test the controls