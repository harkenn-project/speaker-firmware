#include "store.h"

#include <Preferences.h>

namespace store {
namespace {
Preferences prefs;
SemaphoreHandle_t mutex;

struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};
}  // namespace

void begin() {
  mutex = xSemaphoreCreateMutex();
  prefs.begin("kotodama", false);
}

// Preferences logs an error for every missing key, so check first.
String get(const char* key, const char* fallback) {
  return prefs.isKey(key) ? prefs.getString(key, fallback) : String(fallback);
}

String serverUrl() {
  Lock l;
  String url = get("server", KOTODAMA_SERVER_URL);
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  return url;
}

void setServerUrl(const String& url) {
  Lock l;
  prefs.putString("server", url);
}

String token() {
  Lock l;
  return get("token", "");
}

void setToken(const String& token) {
  Lock l;
  prefs.putString("token", token);
}

void clearToken() {
  Lock l;
  prefs.remove("token");
}

std::vector<WifiNetwork> wifiNetworks() {
  Lock l;
  std::vector<WifiNetwork> out;
  const uint8_t n = prefs.getUChar("wifi_n", 0);
  for (uint8_t i = 0; i < n && i < MAX_WIFI; i++) {
    WifiNetwork w{get(("ssid" + String(i)).c_str(), ""), get(("pass" + String(i)).c_str(), "")};
    if (w.ssid.length()) out.push_back(w);
  }
  return out;
}

void saveWifiNetworks(const std::vector<WifiNetwork>& networks) {
  Lock l;
  const size_t n = min(networks.size(), MAX_WIFI);
  for (size_t i = 0; i < MAX_WIFI; i++) {
    const String si = "ssid" + String(i), pi = "pass" + String(i);
    if (i < n) {
      prefs.putString(si.c_str(), networks[i].ssid);
      prefs.putString(pi.c_str(), networks[i].password);
    } else {
      if (prefs.isKey(si.c_str())) prefs.remove(si.c_str());
      if (prefs.isKey(pi.c_str())) prefs.remove(pi.c_str());
    }
  }
  prefs.putUChar("wifi_n", n);
}

bool upsertWifi(const String& ssid, const String& password) {
  auto list = wifiNetworks();
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->ssid == ssid) {
      if (it->password == password) return false;
      list.erase(it);
      break;
    }
  }
  list.insert(list.begin(), WifiNetwork{ssid, password});
  if (list.size() > MAX_WIFI) list.resize(MAX_WIFI);
  saveWifiNetworks(list);
  return true;
}

void requestPortal() {
  Lock l;
  prefs.putBool("portal", true);
}

bool takePortalRequest() {
  Lock l;
  const bool requested = prefs.getBool("portal", false);
  if (requested) prefs.remove("portal");  // only removed when present
  return requested;
}

void factoryReset() {
  Lock l;
  prefs.clear();
}

}  // namespace store
