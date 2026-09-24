/*
  ESP32 BLE Keyboard (HID) -> WS2812B controller
  NimBLE-Arduino 2.x + FastLED

  Controls:
    Arrow Left / Right  -> cycle colors
    Volume Up / Down    -> brightness
    Mute                -> power toggle

  Fixes included:
    - Remembers power state after reboot/power loss
    - Remembers brightness after reboot/power loss
    - Remembers selected color after reboot/power loss
    - Loads saved LED state BEFORE showing LEDs
    - Ignore HID reports unless fully connected/subscribed
    - Ignore stale reports during disconnect
    - Keep LEDs latched on disconnect
    - Prevent held Fn/media keys from repeatedly changing LEDs
    - Prevent held arrow keys from repeatedly cycling colors
    - Clean reconnect/rescan behavior
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
#include <Preferences.h>
#include <string>

// ============================================================
// LED CONFIG
// ============================================================

#define LED_PIN      2
#define NUM_LEDS     144

#define SEG1_START   0
#define SEG1_COUNT   14

#define SEG2_START   60
#define SEG2_COUNT   84

#define LED_TYPE     WS2812B
#define COLOR_ORDER  GRB

CRGB leds[NUM_LEDS];

static const CRGB PRESETS[] = {
  CRGB::Red,
  CRGB::Green,
  CRGB::Blue,
  CRGB::Purple,
  CRGB::Cyan,
  CRGB::Orange,
  CRGB::White,
  CRGB(255, 80, 0),
  CRGB(0, 255, 160),
};

static const size_t PRESET_COUNT =
  sizeof(PRESETS) / sizeof(PRESETS[0]);


// ============================================================
// LED STATE
// ============================================================

// Defaults are only used the VERY FIRST time the ESP32 runs.
// After that, Preferences restores the last saved values.

static bool g_powerOn = false;
static uint8_t g_brightness = 96;
static size_t g_presetIdx = 0;

Preferences prefs;


// ============================================================
// BLE / HID STATE
// ============================================================

static volatile bool g_hidReady = false;
static volatile bool g_needScan = false;

static NimBLEAdvertisedDevice* g_target = nullptr;
static NimBLEClient* g_client = nullptr;


// ============================================================
// KEY PRESS EDGE STATE
// ============================================================

static uint16_t g_lastConsumerUsage = 0;
static uint8_t g_lastArrowKey = 0;


// ============================================================
// BLE HID UUIDs
// ============================================================

static NimBLEUUID UUID_HID_SERVICE((uint16_t)0x1812);
static NimBLEUUID UUID_REPORT_CHAR((uint16_t)0x2A4D);

static const NimBLEAddress ALLOWED_ADDR(
  std::string("fd:e5:18:ff:25:c8"),
  BLE_ADDR_RANDOM
);


// ============================================================
// HID USAGE IDS
// ============================================================

static const uint8_t HID_KEY_ARROW_RIGHT = 0x4F;
static const uint8_t HID_KEY_ARROW_LEFT  = 0x50;

static const uint16_t CONS_VOL_UP   = 0x00E9;
static const uint16_t CONS_VOL_DOWN = 0x00EA;
static const uint16_t CONS_MUTE     = 0x00E2;


// ============================================================
// SAVE / LOAD LED STATE
// ============================================================

static void saveLEDState() {

  prefs.begin("ledstate", false);

  prefs.putBool(
    "power",
    g_powerOn
  );

  prefs.putUChar(
    "brightness",
    g_brightness
  );

  prefs.putUInt(
    "preset",
    (uint32_t)g_presetIdx
  );

  prefs.end();

  Serial.print("Saved LED state: power=");
  Serial.print(g_powerOn ? "ON" : "OFF");

  Serial.print(" brightness=");
  Serial.print(g_brightness);

  Serial.print(" preset=");
  Serial.println(g_presetIdx);
}


static void loadLEDState() {

  prefs.begin("ledstate", true);

  /*
    If no saved value exists yet:
      power      defaults OFF
      brightness defaults 96
      preset     defaults 0
  */

  g_powerOn =
    prefs.getBool("power", false);

  g_brightness =
    prefs.getUChar("brightness", 96);

  g_presetIdx =
    (size_t)prefs.getUInt("preset", 0);

  prefs.end();


  // Safety checks

  if (g_presetIdx >= PRESET_COUNT) {
    g_presetIdx = 0;
  }

  if (g_brightness < 1) {
    g_brightness = 1;
  }


  Serial.print("Loaded LED state: power=");
  Serial.print(g_powerOn ? "ON" : "OFF");

  Serial.print(" brightness=");
  Serial.print(g_brightness);

  Serial.print(" preset=");
  Serial.println(g_presetIdx);
}


// ============================================================
// LED FUNCTIONS
// ============================================================

static void fillSegment(
  int start,
  int count,
  const CRGB& color
) {

  if (count <= 0) {
    return;
  }

  int endExcl = start + count;

  if (start < 0) {
    start = 0;
  }

  if (endExcl > NUM_LEDS) {
    endExcl = NUM_LEDS;
  }

  for (int i = start; i < endExcl; i++) {
    leds[i] = color;
  }
}


static void applyLEDs() {

  // Clear whole internal LED buffer first.
  fill_solid(
    leds,
    NUM_LEDS,
    CRGB::Black
  );


  // Paint selected segments.
  const CRGB color =
    PRESETS[g_presetIdx];

  fillSegment(
    SEG1_START,
    SEG1_COUNT,
    color
  );

  fillSegment(
    SEG2_START,
    SEG2_COUNT,
    color
  );


  // Power off is implemented as brightness 0.
  FastLED.setBrightness(
    g_powerOn
      ? g_brightness
      : 0
  );


  // Send one clean frame.
  FastLED.show();
}


// ============================================================
// CHANGE COLOR
// ============================================================

static void nextPreset(int dir) {

  int32_t idx =
    (int32_t)g_presetIdx + dir;


  if (idx < 0) {
    idx =
      (int32_t)PRESET_COUNT - 1;
  }


  if (idx >= (int32_t)PRESET_COUNT) {
    idx = 0;
  }


  g_presetIdx =
    (size_t)idx;


  // Save new color so reboot remembers it.
  saveLEDState();


  applyLEDs();
}


// ============================================================
// CHANGE BRIGHTNESS
// ============================================================

static void adjustBrightness(int delta) {

  int32_t b =
    (int32_t)g_brightness + delta;


  if (b < 1) {
    b = 1;
  }


  if (b > 255) {
    b = 255;
  }


  // Don't write flash unnecessarily
  // if we're already at the limit.
  if ((uint8_t)b == g_brightness) {
    return;
  }


  g_brightness =
    (uint8_t)b;


  // Save brightness.
  saveLEDState();


  applyLEDs();
}


// ============================================================
// POWER TOGGLE
// ============================================================

static void togglePower() {

  g_powerOn =
    !g_powerOn;


  /*
    IMPORTANT:

    Save OFF immediately when mute is pressed.

    That means if the ESP32 loses power afterward,
    it will remember that it was supposed to stay off.
  */

  saveLEDState();


  applyLEDs();
}


// ============================================================
// KEYBOARD REPORT HELPER
// ============================================================

static bool reportContainsKey(
  const uint8_t* data,
  size_t len,
  uint8_t keycode
) {

  if (len != 8) {
    return false;
  }


  for (size_t i = 2; i < 8; i++) {

    if (data[i] == keycode) {
      return true;
    }
  }


  return false;
}


// ============================================================
// KEYBOARD REPORT HANDLER
// ============================================================

static void handleKeyboardReport(
  const uint8_t* data,
  size_t len
) {

  if (!g_hidReady) {
    return;
  }


  if (len != 8) {
    return;
  }


  bool right =
    reportContainsKey(
      data,
      len,
      HID_KEY_ARROW_RIGHT
    );


  bool left =
    reportContainsKey(
      data,
      len,
      HID_KEY_ARROW_LEFT
    );


  // ----------------------------------------------------------
  // RELEASE
  // ----------------------------------------------------------

  if (!right && !left) {

    g_lastArrowKey = 0;

    return;
  }


  // ----------------------------------------------------------
  // RIGHT
  // ----------------------------------------------------------

  if (right) {

    if (
      g_lastArrowKey !=
      HID_KEY_ARROW_RIGHT
    ) {

      g_lastArrowKey =
        HID_KEY_ARROW_RIGHT;

      nextPreset(+1);
    }

    return;
  }


  // ----------------------------------------------------------
  // LEFT
  // ----------------------------------------------------------

  if (left) {

    if (
      g_lastArrowKey !=
      HID_KEY_ARROW_LEFT
    ) {

      g_lastArrowKey =
        HID_KEY_ARROW_LEFT;

      nextPreset(-1);
    }

    return;
  }
}


// ============================================================
// CONSUMER / MEDIA REPORT HANDLER
// ============================================================

static void handleConsumerReport(
  const uint8_t* data,
  size_t len
) {

  if (!g_hidReady) {
    return;
  }


  if (len != 2) {
    return;
  }


  uint16_t usage =
    (uint16_t)data[0] |
    ((uint16_t)data[1] << 8);


  // ----------------------------------------------------------
  // KEY RELEASE
  // ----------------------------------------------------------

  if (usage == 0) {

    g_lastConsumerUsage = 0;

    return;
  }


  // ----------------------------------------------------------
  // IGNORE HELD/REPEATED FN KEY
  // ----------------------------------------------------------

  if (
    usage ==
    g_lastConsumerUsage
  ) {

    return;
  }


  g_lastConsumerUsage =
    usage;


  // ----------------------------------------------------------
  // VALID MEDIA COMMANDS
  // ----------------------------------------------------------

  switch (usage) {

    case CONS_VOL_UP:

      adjustBrightness(+12);

      break;


    case CONS_VOL_DOWN:

      adjustBrightness(-12);

      break;


    case CONS_MUTE:

      togglePower();

      break;


    default:

      /*
        Unknown Fn/media reports
        do absolutely nothing.
      */

      break;
  }
}


// ============================================================
// SCAN CALLBACKS
// ============================================================

class ScanCB :
  public NimBLEScanCallbacks {

  void onResult(
    const NimBLEAdvertisedDevice* adv
  ) override {


    if (
      !adv->isAdvertisingService(
        UUID_HID_SERVICE
      )
    ) {

      return;
    }


    if (
      adv->getAddress() !=
      ALLOWED_ADDR
    ) {

      return;
    }


    Serial.print(
      "Found keyboard: "
    );

    Serial.println(
      adv->getAddress()
        .toString()
        .c_str()
    );


    if (
      g_target != nullptr
    ) {

      delete g_target;

      g_target = nullptr;
    }


    g_target =
      new NimBLEAdvertisedDevice(
        *adv
      );


    NimBLEDevice
      ::getScan()
      ->stop();
  }
};


// ============================================================
// CLIENT CALLBACKS
// ============================================================

class ClientCB :
  public NimBLEClientCallbacks {

  void onConnect(
    NimBLEClient* client
  ) override {

    Serial.println(
      "BLE connected"
    );


    /*
      HID actions remain disabled
      until subscriptions complete.
    */

    g_hidReady = false;


    g_lastConsumerUsage = 0;
    g_lastArrowKey = 0;
  }


  void onDisconnect(
    NimBLEClient* client,
    int reason
  ) override {

    /*
      Immediately stop accepting
      keyboard commands.
    */

    g_hidReady = false;


    /*
      Clear any held Fn/key state.
    */

    g_lastConsumerUsage = 0;
    g_lastArrowKey = 0;


    Serial.print(
      "BLE disconnected, reason="
    );

    Serial.println(reason);


    /*
      IMPORTANT:

      DO NOT touch the LEDs here.

      The BLE keyboard disconnecting should
      have zero effect on the LED state.

      The current LED state has already been
      saved in flash whenever it changed.
    */


    g_client = nullptr;


    if (
      g_target != nullptr
    ) {

      delete g_target;

      g_target = nullptr;
    }


    g_needScan = true;
  }


  void onAuthenticationComplete(
    NimBLEConnInfo& connInfo
  ) override {


    if (
      !connInfo.isEncrypted()
    ) {

      Serial.println(
        "Pairing/encryption failed"
      );


      g_hidReady = false;

      g_lastConsumerUsage = 0;
      g_lastArrowKey = 0;


      if (
        g_client != nullptr &&
        g_client->isConnected()
      ) {

        g_client->disconnect();
      }


      return;
    }


    Serial.println(
      "BLE encrypted"
    );
  }
};


// ============================================================
// SUBSCRIBE TO HID REPORTS
// ============================================================

static size_t
subscribeToAllReportChars(
  NimBLERemoteService* hid
) {

  const std::vector<
    NimBLERemoteCharacteristic*
  >& chars =
    hid->getCharacteristics(true);


  size_t subCount = 0;


  for (auto* ch : chars) {


    if (
      !ch->getUUID()
        .equals(UUID_REPORT_CHAR)
    ) {

      continue;
    }


    if (!ch->canNotify()) {

      continue;
    }


    bool ok =
      ch->subscribe(

        true,

        [](
          NimBLERemoteCharacteristic* chr,
          uint8_t* data,
          size_t len,
          bool isNotify
        ) {


          /*
            Ignore reports until connection
            is completely ready.
          */

          if (!g_hidReady) {
            return;
          }


          if (
            g_client == nullptr ||
            !g_client->isConnected()
          ) {

            return;
          }


          /*
            Only recognize expected report sizes.

            Weird Fn/device-switch reports
            are ignored.
          */

          if (len == 8) {

            handleKeyboardReport(
              data,
              len
            );

          }

          else if (len == 2) {

            handleConsumerReport(
              data,
              len
            );
          }


          /*
            HID DEBUG

            Uncomment if needed:

            Serial.printf(
              "handle=%u len=%u data=",
              chr->getHandle(),
              (unsigned)len
            );

            for (
              size_t i = 0;
              i < len;
              i++
            ) {
              Serial.printf(
                "%02X ",
                data[i]
              );
            }

            Serial.println();
          */
        }
      );


    Serial.print(
      "Subscribe HID report: "
    );


    Serial.println(
      ok ? "OK" : "FAIL"
    );


    if (ok) {
      subCount++;
    }
  }


  Serial.print(
    "HID subscriptions: "
  );


  Serial.println(
    subCount
  );


  return subCount;
}


// ============================================================
// CONNECT AND SUBSCRIBE
// ============================================================

static bool
connectAndSubscribe() {

  if (
    g_target == nullptr
  ) {

    return false;
  }


  g_hidReady = false;

  g_lastConsumerUsage = 0;
  g_lastArrowKey = 0;


  Serial.print(
    "Connecting to "
  );


  Serial.println(
    g_target
      ->getAddress()
      .toString()
      .c_str()
  );


  NimBLEClient* client =
    NimBLEDevice::createClient();


  if (
    client == nullptr
  ) {

    Serial.println(
      "Could not create BLE client"
    );


    return false;
  }


  client->setClientCallbacks(
    new ClientCB(),
    true
  );


  g_client = client;


  // ----------------------------------------------------------
  // CONNECT
  // ----------------------------------------------------------

  if (
    !client->connect(
      g_target
    )
  ) {

    Serial.println(
      "Connect failed"
    );


    g_hidReady = false;


    NimBLEDevice::deleteClient(
      client
    );


    g_client = nullptr;


    if (
      g_target != nullptr
    ) {

      delete g_target;

      g_target = nullptr;
    }


    g_needScan = true;


    return false;
  }


  // ----------------------------------------------------------
  // FIND HID SERVICE
  // ----------------------------------------------------------

  NimBLERemoteService* hid =
    client->getService(
      UUID_HID_SERVICE
    );


  if (
    hid == nullptr
  ) {

    Serial.println(
      "HID service not found"
    );


    g_hidReady = false;


    client->disconnect();


    return false;
  }


  // ----------------------------------------------------------
  // SUBSCRIBE
  // ----------------------------------------------------------

  size_t subscriptions =
    subscribeToAllReportChars(
      hid
    );


  if (
    subscriptions == 0
  ) {

    Serial.println(
      "No HID reports subscribed"
    );


    g_hidReady = false;


    client->disconnect();


    return false;
  }


  // ----------------------------------------------------------
  // ENABLE HID INPUT
  // ----------------------------------------------------------

  g_lastConsumerUsage = 0;
  g_lastArrowKey = 0;

  g_hidReady = true;


  Serial.println(
    "Ready for keyboard input"
  );


  return true;
}


// ============================================================
// START SCAN
// ============================================================

static void startScan() {

  if (
    g_client != nullptr &&
    g_client->isConnected()
  ) {

    return;
  }


  NimBLEScan* scan =
    NimBLEDevice::getScan();


  if (
    scan->isScanning()
  ) {

    return;
  }


  Serial.println(
    "Scanning for keyboard..."
  );


  scan->start(
    0,
    false,
    true
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  /*
    Get control of LED data pin
    as early as possible.
  */

  pinMode(
    LED_PIN,
    OUTPUT
  );

  digitalWrite(
    LED_PIN,
    LOW
  );


  Serial.begin(115200);


  /*
    Keep this short.

    We want to restore the saved
    LED state quickly after boot.
  */

  delay(50);


  // ----------------------------------------------------------
  // FASTLED
  // ----------------------------------------------------------

  FastLED.addLeds<
    LED_TYPE,
    LED_PIN,
    COLOR_ORDER
  >(
    leds,
    NUM_LEDS
  );


  /*
    IMPORTANT:

    Load saved state BEFORE sending
    anything to the LEDs.
  */

  loadLEDState();


  /*
    Clear RAM only.

    FALSE means:
    do NOT transmit the clear command yet.
  */

  FastLED.clear(false);


  /*
    Now send exactly the state
    that was previously saved.
  */

  applyLEDs();


  // ----------------------------------------------------------
  // NIMBLE
  // ----------------------------------------------------------

  NimBLEDevice::init(
    "ESP32-HID-LED"
  );


  NimBLEDevice::setPower(9);


  NimBLEDevice::setSecurityAuth(
    true,
    false,
    true
  );


  NimBLEDevice::setSecurityIOCap(
    BLE_HS_IO_NO_INPUT_OUTPUT
  );


  // ----------------------------------------------------------
  // SCANNER
  // ----------------------------------------------------------

  NimBLEScan* scan =
    NimBLEDevice::getScan();


  scan->setScanCallbacks(
    new ScanCB(),
    true
  );


  scan->setActiveScan(true);


  scan->setInterval(45);
  scan->setWindow(30);


  Serial.println();

  Serial.println(
    "ESP32 HID LED Controller"
  );


  Serial.println(
    "Scanning for keyboard..."
  );


  scan->start(
    0,
    false,
    true
  );
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  /*
    Restart scanning after disconnect.
  */

  if (g_needScan) {

    g_needScan = false;


    delay(100);


    startScan();
  }


  /*
    Connect when our keyboard
    advertisement is found.
  */

  if (
    g_target != nullptr &&
    (
      g_client == nullptr ||
      !g_client->isConnected()
    )
  ) {

    connectAndSubscribe();
  }


  delay(10);
}