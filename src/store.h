// Persistent settings in NVS: server URL, device token, Wi-Fi networks.
#pragma once
#include <Arduino.h>
#include <vector>

namespace store {

struct WifiNetwork {
  String ssid;
  String password;
};
constexpr size_t MAX_WIFI = 8;

void begin();

// The server's address: entered on the setup page, or the one the speaker was adopted through. Empty: the speaker
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
bool upsertWifi(const String& ssid, const String& password);

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

// Ask for the Wi-Fi setup portal on the next boot (saved networks are kept).
void requestPortal();
bool takePortalRequest();  // true once after requestPortal()

void factoryReset();  // forget everything: Wi-Fi, token, server URL

}  // namespace store
