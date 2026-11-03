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
  prefs.begin("harkenn", false);
}

// Preferences logs an error for every missing key, so check first.
String get(const char* key, const char* fallback) {
  return prefs.isKey(key) ? prefs.getString(key, fallback) : String(fallback);
}

String clean_url(String url) {
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  return url;
}

void put_or_remove(const char* key, const String& value) {
  if (value.length()) prefs.putString(key, value);
  else if (prefs.isKey(key)) prefs.remove(key);
}

String serverUrl() {
  Lock l;
  return clean_url(get("server", ""));
}

void setServerUrl(const String& url) {
  Lock l;
  put_or_remove("server", clean_url(url));
}

String announcedServerUrl() {
  Lock l;
  return clean_url(get("server_alt", ""));
}

void setAnnouncedServerUrl(const String& url) {
  Lock l;
  put_or_remove("server_alt", clean_url(url));
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
    WifiNetwork w;
    w.ssid = get(("ssid" + String(i)).c_str(), "");
    w.password = get(("pass" + String(i)).c_str(), "");
    // static IP, as "ip,subnet,gateway,dns"
    const String st = get(("ip" + String(i)).c_str(), "");
    const int a = st.indexOf(','), b = st.indexOf(',', a + 1), c = st.indexOf(',', b + 1);
    if (a > 0 && b > a && c > b) {
      w.ip = st.substring(0, a);
      w.subnet = st.substring(a + 1, b);
      w.gateway = st.substring(b + 1, c);
      w.dns = st.substring(c + 1);
    }
    if (w.ssid.length()) out.push_back(w);
  }
  return out;
}

void saveWifiNetworks(const std::vector<WifiNetwork>& networks) {
  Lock l;
  const size_t n = min(networks.size(), MAX_WIFI);
  for (size_t i = 0; i < MAX_WIFI; i++) {
    const String si = "ssid" + String(i), pi = "pass" + String(i), ii = "ip" + String(i);
    if (i < n) {
      const WifiNetwork& w = networks[i];
      prefs.putString(si.c_str(), w.ssid);
      prefs.putString(pi.c_str(), w.password);
      put_or_remove(ii.c_str(), w.ip.length() ? w.ip + "," + w.subnet + "," + w.gateway + "," + w.dns : String());
    } else {
      for (const String& k : {si, pi, ii}) {
        if (prefs.isKey(k.c_str())) prefs.remove(k.c_str());
      }
    }
  }
  prefs.putUChar("wifi_n", n);
}

bool upsertWifi(const WifiNetwork& network) {
  auto list = wifiNetworks();
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->ssid == network.ssid) {
      if (*it == network) return false;
      list.erase(it);
      break;
    }
  }
  list.insert(list.begin(), network);
  if (list.size() > MAX_WIFI) list.resize(MAX_WIFI);
  saveWifiNetworks(list);
  return true;
}

bool removeWifi(const String& ssid) {
  auto list = wifiNetworks();
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->ssid == ssid) {
      list.erase(it);
      saveWifiNetworks(list);
      return true;
    }
  }
  return false;
}

int volume(int fallback) {
  Lock l;
  return prefs.isKey("volume") ? prefs.getChar("volume", fallback) : fallback;
}

void setVolume(int volume) {
  Lock l;
  prefs.putChar("volume", volume);
}

int configVolume() {
  Lock l;
  return prefs.isKey("cfg_vol") ? prefs.getChar("cfg_vol", -1) : -1;
}

void setConfigVolume(int volume) {
  Lock l;
  prefs.putChar("cfg_vol", volume);
}

bool micMuted() {
  Lock l;
  return prefs.isKey("muted") && prefs.getBool("muted", false);
}

void setMicMuted(bool muted) {
  Lock l;
  prefs.putBool("muted", muted);
}

void setUpdatedFrom(const String& version) {
  Lock l;
  prefs.putString("ota_from", version);
}

String takeUpdatedFrom() {
  Lock l;
  const String v = get("ota_from", "");
  if (v.length()) prefs.remove("ota_from");
  return v;
}

String dismissedUpdate() {
  Lock l;
  return get("dismissed_fw", "");
}

void setDismissedUpdate(const String& version) {
  Lock l;
  prefs.putString("dismissed_fw", version);
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

LocalAccess localAccess() {
  Lock l;
  LocalAccess a;
  a.admin_hash = get("adm_hash", "");
  a.speaker_hash = get("spk_hash", "");
  a.hotspot_psk = get("ap_psk", "");
  a.unlock = prefs.isKey("unlock") ? prefs.getInt("unlock", 0) : 0;
  return a;
}

void setLocalAccess(const LocalAccess& a) {
  Lock l;
  put_or_remove("adm_hash", a.admin_hash);
  put_or_remove("spk_hash", a.speaker_hash);
  put_or_remove("ap_psk", a.hotspot_psk);
  prefs.putInt("unlock", a.unlock);
}

uint8_t loginFailures() {
  Lock l;
  return prefs.isKey("lk_fail") ? prefs.getUChar("lk_fail", 0) : 0;
}

void setLoginFailures(uint8_t failures) {
  Lock l;
  prefs.putUChar("lk_fail", failures);
}

bool loginLocked() {
  Lock l;
  return prefs.isKey("lk_perm") && prefs.getBool("lk_perm", false);
}

void setLoginLocked(bool locked) {
  Lock l;
  prefs.putBool("lk_perm", locked);
}

String localChanges() {
  Lock l;
  return get("local_chg", "");
}

uint32_t localChangesId() {
  Lock l;
  return prefs.isKey("local_id") ? prefs.getUInt("local_id", 0) : 0;
}

void setLocalChanges(const String& json) {
  Lock l;
  prefs.putString("local_chg", json);
  prefs.putUInt("local_id", (prefs.isKey("local_id") ? prefs.getUInt("local_id", 0) : 0) + 1);
}

void clearLocalChanges(uint32_t id) {
  Lock l;
  if (!prefs.isKey("local_id") || prefs.getUInt("local_id", 0) != id) return;
  if (prefs.isKey("local_chg")) prefs.remove("local_chg");
}

void factoryReset() {
  Lock l;
  prefs.clear();
}

}  // namespace store
