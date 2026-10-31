// Kotodama speaker firmware. The protocol it speaks is described in ../../docs/PROTOCOL.md.
#include <Arduino.h>

#include "app.h"
#include "board.h"
#include "logbuf.h"
#include "store.h"
#include "ui.h"

// Kotodama reads the version and board of an uploaded firmware image from this marker (app/firmware.py).
extern "C" const char KOTODAMA_FW_MARKER[] = "KOTODAMA-FW|" FW_VERSION "|" HARDWARE_ID "|";

void setup() {
  Serial.begin(115200);
  Serial.println(KOTODAMA_FW_MARKER);  // also keeps the marker from being dropped by the linker
  logbuf::begin();
  store::begin();
  board::begin();
  ui::begin();
  app::start();
}

void loop() { ui::loop(); }
