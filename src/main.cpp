// Harkenn speaker firmware. The protocol it speaks is described in docs/PROTOCOL.md in server-docker.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include "app.h"
#include "board.h"
#include "logbuf.h"
#include "store.h"
#include "ui.h"

// Harkenn reads the version and board of an uploaded firmware image from this marker (app/firmware.py in server-docker).
extern "C" const char HARKENN_FW_MARKER[] = "HARKENN-FW|" FW_VERSION "|" HARDWARE_ID "|";

// TLS memory in PSRAM: the framework's mbedTLS uses internal RAM, about 30-40 KB per secure connection, which left too
// little for anything else (a second connection, the web page). Internal RAM only if PSRAM runs out.
void* tls_calloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void setup() {
  mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
  Serial.begin(115200);
  Serial.println(HARKENN_FW_MARKER);  // also keeps the marker from being dropped by the linker
  logbuf::begin();
  store::begin();
  board::begin();
  ui::begin();
  app::start();
}

void loop() { ui::loop(); }
