#pragma once
#include <Arduino.h>

namespace portal {
// Runs the Wi-Fi setup access point until the user saves a network, then restarts. Never returns.
[[noreturn]] void run(const String& apName);
}
