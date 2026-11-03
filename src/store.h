// Persistent settings in NVS: server URL, device token, Wi-Fi networks, the web page's passwords.
#pragma once
#include <Arduino.h>
#include <vector>

namespace store {

struct WifiNetwork {
  String ssid;
  String password;
  String ip, gateway, subnet, dns;  // static IP on this network; empty ip = DHCP
  bool operator==(const WifiNetwork& o) const {
    return ssid == o.ssid && password == o.password && ip == o.ip && gateway == o.gateway && subnet == o.subnet &&
           dns == o.dns;
  }
};
constexpr size_t MAX_WIFI = 8;

void begin();

// The server's address: entered on the web page, or the one the speaker was adopted through. Empty: the speaker
// looks for its server (app.cpp). Setting an empty string forgets it.
String serverUrl();
void setServerUrl(const String& url);
// The public address the server announces in its config: tried second, so a wrong value can't strand the speaker.
String announcedServerUrl();
void setAnnouncedServerUrl(const String& url);

String token();
void setToken(const String& token);
void clearToken();

std::vector<WifiNetwork> wifiNetworks();
void saveWifiNetworks(const std::vector<WifiNetwork>& networks);
// Adds or updates a network; the most recently added goes first. Returns true if anything changed.
bool upsertWifi(const WifiNetwork& network);
bool removeWifi(const String& ssid);  // true if it was saved

// Audio settings changed on the speaker itself. `configVolume` is the volume of the last config applied, so a config
// only overrides the local volume when the volume in it changed.
int volume(int fallback);
void setVolume(int volume);
int configVolume();  // -1 if none yet
void setConfigVolume(int volume);
bool micMuted();
void setMicMuted(bool muted);

// The firmware version installed before an over-the-air update, reported once after the restart.
void setUpdatedFrom(const String& version);
String takeUpdatedFrom();  // empty if there was no update

// The firmware version the user dismissed on the "Update available" prompt; it isn't offered again.
String dismissedUpdate();
void setDismissedUpdate(const String& version);

// Ask for the hotspot and web page on the next boot (Wi-Fi setup from the button; saved networks are kept).
void requestPortal();
bool takePortalRequest();  // true once after requestPortal()

// The speaker's own web page (web.cpp). Hashes come from Harkenn's config ("local_access"): the admin password's
// and the speaker password's ("pbkdf2_sha256$rounds$salt$digest", base64), and the hotspot's WPA2 key (64 hex
// digits, itself derived from the speaker password).
struct LocalAccess {
  String admin_hash, speaker_hash, hotspot_psk;
  int unlock = 0;  // Harkenn's unlock counter: a change clears the lockout
};
LocalAccess localAccess();
void setLocalAccess(const LocalAccess& access);
// Wrong passwords since the last right one, and whether the page is locked until Harkenn unlocks it.
uint8_t loginFailures();
void setLoginFailures(uint8_t failures);
bool loginLocked();
void setLoginLocked(bool locked);

// Remote connection through the relay, from Harkenn's config ("remote"): its address (wss://...), this speaker's
// relay id and key (hex). Empty url = not set up for it.
struct Remote {
  String url, id, key;
};
Remote remote();
void setRemote(const Remote& remote);

// Settings changed on the web page that Harkenn hasn't confirmed yet, as JSON (wifi, wifi_removed, log_level,
// brightness, server_url). They win over the config until Harkenn acknowledges them by id.
String localChanges();
uint32_t localChangesId();
void setLocalChanges(const String& json);  // gets a new id
void clearLocalChanges(uint32_t id);       // only if nothing changed since that id

void factoryReset();  // forget everything: Wi-Fi, token, server URL, passwords

}  // namespace store
