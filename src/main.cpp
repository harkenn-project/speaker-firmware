// Harkenn speaker firmware. The protocol it speaks is described in docs/PROTOCOL.md in harkenn-dockerserver.
#include <Arduino.h>

#include "app.h"
#include "board.h"
#include "logbuf.h"
#include "store.h"
#include "ui.h"

// Harkenn reads the version and board of an uploaded firmware image from this marker (app/firmware.py in harkenn-dockerserver).
extern "C" const char HARKENN_FW_MARKER[] = "HARKENN-FW|" FW_VERSION "|" HARDWARE_ID "|";

void setup() {
  Serial.begin(115200);
  Serial.println(HARKENN_FW_MARKER);  // also keeps the marker from being dropped by the linker
  logbuf::begin();
  store::begin();
  board::begin();
  ui::begin();
  app::start();
}

void loop() { ui::loop(); }
