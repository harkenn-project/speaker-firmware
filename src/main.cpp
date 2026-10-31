// Kotodama speaker firmware. The protocol it speaks is described in ../../docs/PROTOCOL.md.
#include <Arduino.h>

#include "app.h"
#include "board.h"
#include "logbuf.h"
#include "store.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);
  logbuf::begin();
  store::begin();
  board::begin();
  ui::begin();
  app::start();
}

void loop() { ui::loop(); }
