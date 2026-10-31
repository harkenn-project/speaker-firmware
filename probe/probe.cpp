// Hardware probe (env:probe): reports which unassigned GPIOs are driven and logs every change, so flipping a switch
// shows up on the serial console. Also scans both I2C buses. Not part of the speaker firmware.
#include <Arduino.h>
#include <Wire.h>

namespace {

// GPIOs not used by the display, touch, audio, flash/PSRAM (26-37), USB (19/20) or the console UART (43/44).
constexpr int CANDIDATES[] = {0, 1, 3, 13, 17, 18, 21, 39, 40, 41, 48};
constexpr size_t N = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

// 'H' or 'L' when something drives the pin (same reading with pull-up and pull-down), 'f' when it floats.
char sample(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(200);
  const int up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(200);
  const int down = digitalRead(pin);
  if (up != down) return 'f';
  return up ? 'H' : 'L';
}

void scan(TwoWire& bus, const char* name) {
  Serial.printf("I2C %s:", name);
  for (uint8_t a = 1; a < 127; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) Serial.printf(" 0x%02x", a);
  }
  Serial.println();
}

char last[N];
int last_adc = -1;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(46, OUTPUT);
  digitalWrite(46, LOW);
  Wire.begin(15, 14);
  Wire1.begin(11, 7);
  scan(Wire, "codec (15/14)");
  scan(Wire1, "touch (11/7)");
  Serial.print("GPIO:");
  for (size_t i = 0; i < N; i++) {
    last[i] = sample(CANDIDATES[i]);
    Serial.printf(" %d=%c", CANDIDATES[i], last[i]);
  }
  Serial.println();
  Serial.println("PROBE READY - flip the switch now");
}

void loop() {
  for (size_t i = 0; i < N; i++) {
    if (CANDIDATES[i] == 1) continue;  // battery ADC, reported below
    const char v = sample(CANDIDATES[i]);
    if (v != last[i]) {
      Serial.printf("[%lu] GPIO%d %c -> %c\n", millis(), CANDIDATES[i], last[i], v);
      last[i] = v;
    }
  }
  const int mv = analogReadMilliVolts(1);
  if (last_adc < 0 || abs(mv - last_adc) > 150) {
    Serial.printf("[%lu] battery ADC %d mV\n", millis(), mv);
    last_adc = mv;
  }
  delay(20);
}
