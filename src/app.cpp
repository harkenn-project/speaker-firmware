#include "app.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <esp_mac.h>
#include <sys/time.h>

#include <map>
#include <memory>

#include "audio.h"
#include "board.h"
#include "ca_certs.h"
#include "logbuf.h"
#include "portal.h"
#include "store.h"
#include "ui.h"

namespace app {
namespace {

constexpr const char* TAG = "app";
constexpr uint32_t STATUS_INTERVAL_MS = 30000;
constexpr uint32_t LOG_FLUSH_MS = 2000;
constexpr uint32_t WS_GIVE_UP_MS = 30000;  // then re-check the token over HTTP
constexpr size_t MAX_ASSET_BYTES = 3 * 1024 * 1024;
constexpr uint32_t MAX_RECORD_MS = 8000;
constexpr uint32_t CHIME_MS = 350;          // start recording after the activation chime, so it isn't recorded
constexpr uint32_t VOLUME_SETTLE_MS = 1000;  // save and report the volume once the finger stops

struct ServerUrl {
  bool secure = true;
  String host;
  uint16_t port = 443;
  String base;  // path prefix, without trailing slash
};

ServerUrl parse_url(const String& url) {
  ServerUrl u;
  String rest = url;
  if (url.startsWith("https://")) {
    rest = url.substring(8);
  } else {
    u.secure = false;
    u.port = 80;
    if (url.startsWith("http://")) rest = url.substring(7);
  }
  const int slash = rest.indexOf('/');
  const String hostport = slash >= 0 ? rest.substring(0, slash) : rest;
  u.base = slash >= 0 ? rest.substring(slash) : "";
  while (u.base.endsWith("/")) u.base.remove(u.base.length() - 1);
  const int colon = hostport.indexOf(':');
  u.host = colon >= 0 ? hostport.substring(0, colon) : hostport;
  if (colon >= 0) u.port = hostport.substring(colon + 1).toInt();
  return u;
}

String device_id;  // Wi-Fi MAC as 12 lowercase hex digits
String server_url;
ServerUrl server;
String token;
String timezone = "UTC0";
WiFiMulti* wifi = nullptr;

// Applied configuration
int config_version = 0;
int brightness = 75;
volatile int volume = 70;
uint32_t sleep_after_ms = 300000;
bool dimmed = false;
std::map<String, std::shared_ptr<ui::Blob>> blobs;  // animations by URL, so unchanged ones aren't downloaded again

// Live connection
WebSocketsClient ws;
bool ws_connected = false;
uint32_t ws_down_since = 0;
String pending_config;  // config received over the socket, applied from the main loop
bool first_ws_config = false;
String applied_key;  // config_key() of the config on screen
bool want_config = false;
uint32_t last_status = 0, last_log_flush = 0;
String play_state_restore;
uint32_t play_state_until = 0;

// Touch, button and voice
volatile bool tap_requested = false;
volatile bool status_dirty = false;
volatile uint32_t volume_changed_at = 0;
uint32_t status_text_until = 0;
enum class Voice { Off, Starting, Recording };
Voice voice = Voice::Off;
uint32_t voice_at = 0;

String ap_name() {
  String suffix = device_id.substring(8);
  suffix.toUpperCase();
  return "Kotodama-" + suffix;
}

std::unique_ptr<WiFiClient> make_client(const String& url) {
  if (url.startsWith("https://")) {
    auto* c = new WiFiClientSecure();
    c->setCACert(CA_PEM);
    return std::unique_ptr<WiFiClient>(c);
  }
  return std::unique_ptr<WiFiClient>(new WiFiClient());
}

// Returns the HTTP status (negative if the connection failed); fills `response` when given.
int request(const char* method, const String& path, const String& body, String* response, bool auth) {
  const String url = server_url + path;
  auto client = make_client(url);
  HTTPClient http;  // declared after `client` so it is destroyed first
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  if (!http.begin(*client, url)) return -1;
  if (auth) http.addHeader("Authorization", "Bearer " + token);
  int code;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  if (code > 0 && response) *response = http.getString();
  http.end();
  return code;
}

int upload_recording(uint8_t* wav, size_t size, const char* kind) {
  const String url = server_url + "/api/v1/device/recordings?kind=" + kind;
  auto client = make_client(url);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(20000);
  if (!http.begin(*client, url)) return -1;
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Content-Type", "audio/wav");
  const int code = http.POST(wav, size);
  http.end();
  return code;
}

std::shared_ptr<ui::Blob> download(const String& url) {
  auto client = make_client(url);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  if (!http.begin(*client, url)) return nullptr;
  const int code = http.GET();
  const int len = http.getSize();
  if (code != 200 || len <= 0 || static_cast<size_t>(len) > MAX_ASSET_BYTES) {
    LOGW(TAG, "download %s: HTTP %d, %d bytes", url.c_str(), code, len);
    http.end();
    return nullptr;
  }
  auto blob = std::make_shared<ui::Blob>();
  blob->data = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
  if (!blob->data) {
    LOGE(TAG, "out of PSRAM for %d byte animation", len);
    http.end();
    return nullptr;
  }
  blob->size = len;
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  const uint32_t start = millis();
  while (got < blob->size && millis() - start < 30000) {
    const int avail = stream->available();
    if (avail > 0) {
      got += stream->readBytes(blob->data + got, min(static_cast<size_t>(avail), blob->size - got));
    } else if (!stream->connected()) {
      break;
    } else {
      delay(2);
    }
  }
  http.end();
  if (got != blob->size) {
    LOGW(TAG, "download %s: got %u of %u bytes", url.c_str(), got, blob->size);
    return nullptr;
  }
  return blob;
}

// Wi-Fi

void rebuild_wifi() {
  delete wifi;
  wifi = new WiFiMulti();
  for (const auto& n : store::wifiNetworks()) {
    wifi->addAP(n.ssid.c_str(), n.password.length() ? n.password.c_str() : nullptr);
  }
}

void connect_wifi() {
  const auto networks = store::wifiNetworks();
  if (networks.empty()) portal::run(ap_name());
  ui::showMessage("Connecting to Wi-Fi", networks.front().ssid.c_str());
  WiFi.setHostname(("kotodama-" + device_id.substring(8)).c_str());
  WiFi.mode(WIFI_STA);
  int attempts = 0;
  while (wifi->run(10000) != WL_CONNECTED) {
    if (++attempts == 6) {
      ui::showMessage("Can't join Wi-Fi", "Still trying. Hold the button 5 s for Wi-Fi setup.");
    }
    delay(500);
  }
  LOGI(TAG, "Wi-Fi: %s, IP %s, RSSI %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  configTzTime(timezone.c_str(), "pool.ntp.org", "time.google.com");
}

// Server discovery and adoption

bool find_server() {
  ui::showMessage("Looking for Kotodama", server.host.c_str());
  while (WiFi.status() == WL_CONNECTED) {
    String body;
    const int code = request("GET", "/api/v1/ping", "", &body, false);
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, body) && time(nullptr) < 1700000000) {
        const double t = doc["time"] | 0.0;  // set the clock now rather than waiting for NTP
        if (t > 1.7e9) {
          timeval tv{static_cast<time_t>(t), 0};
          settimeofday(&tv, nullptr);
        }
      }
      LOGI(TAG, "found Kotodama at %s", server_url.c_str());
      return true;
    }
    LOGW(TAG, "ping %s: %d", server_url.c_str(), code);
    delay(3000);
  }
  return false;
}

bool adopt() {
  JsonDocument req;
  req["device_id"] = device_id;
  req["hardware"] = HARDWARE_ID;
  req["firmware"] = FW_VERSION;
  String body, resp;
  serializeJson(req, body);
  const int code = request("POST", "/api/v1/adoption/request", body, &resp, false);
  if (code != 200) {
    LOGW(TAG, "adoption request: HTTP %d", code);
    delay(5000);
    return false;
  }
  JsonDocument r;
  deserializeJson(r, resp);
  const String id = r["request_id"] | "";
  const String adoption_code = r["code"] | "";
  const int poll_s = max(1, r["poll_interval_s"] | 3);
  ui::showAdoptionCode(adoption_code.c_str(), server.host.c_str());
  LOGI(TAG, "waiting to be adopted");

  int failures = 0;
  while (WiFi.status() == WL_CONNECTED) {
    delay(poll_s * 1000);
    String s;
    const int sc = request("GET", "/api/v1/adoption/" + id, "", &s, false);
    if (sc == 404) return false;
    if (sc != 200) {
      if (++failures > 20) return false;
      continue;
    }
    failures = 0;
    JsonDocument st;
    deserializeJson(st, s);
    const String state = st["state"] | "";
    if (state == "pending") continue;
    if (state == "approved") {
      token = st["token"] | "";
      store::setToken(token);
      LOGI(TAG, "adopted");
      ui::showMessage("Adopted!", "Loading settings");
      return true;
    }
    LOGI(TAG, "adoption request %s; asking again", state.c_str());
    return false;
  }
  return false;
}

// Configuration

// Identifies what a config would change: the version, plus the character manifest (its URLs carry the asset
// version and background colour, which can change with a server upgrade even when the config version does not).
String config_key(JsonDocument& doc) {
  String key = String(doc["version"] | 0) + "|";
  serializeJson(doc["character"], key);
  return key;
}

uint32_t hex_color(JsonVariantConst v, uint32_t fallback) {
  const char* s = v | "";
  return (s[0] == '#' && strlen(s) == 7) ? strtoul(s + 1, nullptr, 16) : fallback;
}

void apply_config(JsonDocument& doc) {
  JsonObject cfg = doc["config"];
  JsonObject display = cfg["display"];

  ui::Theme theme;
  JsonObject th = doc["theme"];
  theme.background = hex_color(th["background"], theme.background);
  theme.surface = hex_color(th["surface"], theme.surface);
  theme.primary = hex_color(th["primary"], theme.primary);
  theme.accent = hex_color(th["accent"], theme.accent);
  theme.text = hex_color(th["text"], theme.text);
  theme.muted = hex_color(th["muted"], theme.muted);

  std::vector<ui::Widget> widgets;
  int character_size = 0;
  for (JsonObject w : doc["layout"]["widgets"].as<JsonArray>()) {
    ui::Widget x;
    x.type = w["type"] | "";
    x.x = w["x"] | 0;
    x.y = w["y"] | 0;
    x.w = w["w"] | 0;
    x.h = w["h"] | 0;
    x.font = w["font"] | "md";
    x.lines = w["lines"] | 1;
    x.thickness = w["thickness"] | 4;
    if (x.type == "character") character_size = x.w;
    widgets.push_back(x);
  }

  // Character animations: download what changed, reuse the rest.
  ui::CharacterSet set;
  std::map<String, std::shared_ptr<ui::Blob>> next;
  if (character_size) {
    JsonObject states = doc["character"]["states"];
    for (size_t i = 0; i < ui::STATE_COUNT; i++) {
      const String url = states[ui::STATES[i]]["urls"][String(character_size)] | "";
      if (!url.length()) continue;
      auto have = next.find(url);
      if (have == next.end()) {
        auto old = blobs.find(url);
        auto blob = old != blobs.end() ? old->second : download(url);
        if (!blob) continue;
        have = next.emplace(url, blob).first;
      }
      set[i] = have->second;
    }
  }
  blobs = next;

  brightness = display["brightness"] | 75;
  sleep_after_ms = static_cast<uint32_t>(display["sleep_after_s"] | 300) * 1000;
  // The speaker's own volume (set on the touch screen) wins unless the volume in the config was changed.
  const int config_volume = constrain(cfg["audio"]["volume"] | 70, 0, 100);
  if (config_volume != store::configVolume()) {
    store::setConfigVolume(config_volume);
    volume = config_volume;
    store::setVolume(volume);
    audio::setVolume(volume);
  }
  timezone = cfg["timezone"] | "UTC0";
  setenv("TZ", timezone.c_str(), 1);
  tzset();
  const String level = cfg["log_level"] | "info";
  logbuf::setUploadLevel(level == "error" ? 'E' : level == "warn" ? 'W' : level == "debug" ? 'D' : 'I');

  // Wi-Fi networks from the server are added to the saved list; nothing is ever removed from here.
  bool wifi_changed = false;
  for (JsonObject n : cfg["wifi"].as<JsonArray>()) {
    const String ssid = n["ssid"] | "";
    if (ssid.length()) wifi_changed |= store::upsertWifi(ssid, n["password"] | "");
  }
  if (wifi_changed) {
    rebuild_wifi();
    LOGI(TAG, "Wi-Fi list updated: %u networks", store::wifiNetworks().size());
  }

  ui::setCharacter(set);
  ui::showLayout(theme, widgets, display["clock_24h"] | true);
  dimmed = false;
  board::markActivity();
  board::setBrightness(brightness);
  config_version = doc["version"] | 0;
  applied_key = config_key(doc);
  LOGI(TAG, "applied config v%d (layout %s, character %s)", config_version, (const char*)(doc["layout"]["id"] | "?"),
       (const char*)(doc["character"]["id"] | "?"));
}

// Live connection

void send_json(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);
}

void send_status() {
  last_status = millis();
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "status";
  JsonObject s = doc["status"].to<JsonObject>();
  s["state"] = ui::state();
  s["firmware"] = FW_VERSION;
  s["ip"] = WiFi.localIP().toString();
  s["ssid"] = WiFi.SSID();
  s["rssi"] = WiFi.RSSI();
  s["uptime_s"] = millis() / 1000;
  s["free_heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s["free_psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s["battery"] = board::batteryPercent();
  s["charging"] = board::charging();
  s["volume"] = volume;
  s["muted"] = audio::micMuted();
  s["brightness"] = brightness;
  s["config_version"] = config_version;
  send_json(doc);
}

// Tells Kotodama about settings changed on the speaker, so its stored config matches.
void send_settings() {
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "settings";
  doc["volume"] = volume;
  send_json(doc);
}

void set_volume_local(int v) {
  volume = constrain(v, 0, 100);
  audio::setVolume(volume);
  volume_changed_at = millis();
}

// Voice: a tap starts listening; for now the recording is uploaded to Kotodama (Recordings in the admin UI).

void flash_state(const char* state, uint32_t ms) {
  if (!play_state_until) play_state_restore = "idle";
  ui::setState(state);
  play_state_until = millis() + ms;
}

void show_status_text(const char* text, uint32_t ms) {
  ui::setStatusText(text);
  status_text_until = millis() + ms;
}

void finish_voice() {
  voice = Voice::Off;
  size_t size = 0;
  uint32_t ms = 0;
  int peak = 0;
  uint8_t* wav = audio::takeRecordingWav(&size, &ms, &peak);
  if (!wav || audio::micMuted()) {  // muted mid-recording: throw it away
    if (wav) heap_caps_free(wav);
    ui::setState("idle");
    return;
  }
  audio::play(audio::Sound::Done);
  ui::setState("thinking");
  const int code = upload_recording(wav, size, "utterance");
  heap_caps_free(wav);
  LOGI(TAG, "recorded %u ms, peak %d%%, upload HTTP %d", ms, peak, code);
  if (code == 201) {
    ui::setState("idle");
  } else {
    audio::play(audio::Sound::Error);
    flash_state("error", 3000);
  }
}

void handle_tap() {
  if (voice == Voice::Recording) {
    audio::stopRecording();  // finish_voice() picks it up
  } else if (voice == Voice::Starting) {
    voice = Voice::Off;
    audio::play(audio::Sound::Cancel);
    ui::setState("idle");
  } else if (audio::micMuted()) {
    audio::play(audio::Sound::Cancel);
    show_status_text("Mic is off", 2500);
  } else if (strcmp(ui::state(), "idle") == 0 || strcmp(ui::state(), "sleeping") == 0) {
    audio::play(audio::Sound::Activate);
    ui::setState("listening");
    voice = Voice::Starting;
    voice_at = millis() + CHIME_MS;
  }
}

void update_voice() {
  if (tap_requested) {
    tap_requested = false;
    handle_tap();
  }
  if (voice == Voice::Starting && audio::micMuted()) {  // muted before recording began
    voice = Voice::Off;
    ui::setState("idle");
  } else if (voice == Voice::Starting && static_cast<int32_t>(millis() - voice_at) >= 0) {
    if (audio::startRecording(MAX_RECORD_MS)) {
      voice = Voice::Recording;
    } else {
      voice = Voice::Off;
      audio::play(audio::Sound::Error);
      flash_state("error", 3000);
    }
  } else if (voice == Voice::Recording && !audio::recording()) {
    finish_voice();
  }
  if (status_text_until && static_cast<int32_t>(millis() - status_text_until) >= 0) {
    status_text_until = 0;
    ui::setStatusText("");
  }
  if (volume_changed_at && millis() - volume_changed_at > VOLUME_SETTLE_MS) {
    volume_changed_at = 0;
    store::setVolume(volume);
    send_settings();
    send_status();
  }
  if (status_dirty) {
    status_dirty = false;
    send_status();
  }
}

void flush_logs() {
  last_log_flush = millis();
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "log";
  if (logbuf::drain(doc["lines"].to<JsonArray>(), 40)) send_json(doc);
}

void handle_text(const uint8_t* payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const String type = doc["type"] | "";
  if (type == "config") {
    pending_config = "";
    pending_config.concat(reinterpret_cast<const char*>(payload), len);
  } else if (type == "ping") {
    ws.sendTXT("{\"type\":\"pong\"}");
  } else if (type == "command") {
    const String command = doc["command"] | "";
    JsonObject args = doc["args"];
    LOGI(TAG, "command %s", command.c_str());
    if (command == "reboot") {
      delay(300);
      ESP.restart();
    } else if (command == "identify") {
      ui::identify();
    } else if (command == "refresh_config") {
      want_config = true;
    } else if (command == "set_volume") {
      set_volume_local(args["volume"] | static_cast<int>(volume));
      audio::play(audio::Sound::Tick);
    } else if (command == "play_state") {
      const String state = args["state"] | "idle";
      if (ui::stateIndex(state.c_str()) >= 0) {
        if (!play_state_until) play_state_restore = ui::state();
        ui::setState(state.c_str());
        play_state_until = millis() + 1000 * constrain(args["seconds"] | 5, 1, 60);
        send_status();
      }
    }
  }
}

void on_ws_event(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      ws_connected = true;
      ws_down_since = 0;
      first_ws_config = true;
      LOGI(TAG, "connected to Kotodama");
      send_settings();  // in case the volume changed while offline
      send_status();
      break;
    case WStype_DISCONNECTED:
      if (ws_connected) LOGW(TAG, "connection to Kotodama lost");
      ws_connected = false;
      if (!ws_down_since) ws_down_since = millis();
      break;
    case WStype_TEXT:
      handle_text(payload, len);
      break;
    default:
      break;
  }
}

void update_sleep() {
  if (!sleep_after_ms) return;
  const bool idle = millis() - board::lastTouchMs() > sleep_after_ms;
  if (idle && !dimmed) {
    dimmed = true;
    board::setBrightness(max(5, brightness / 8));
    if (strcmp(ui::state(), "idle") == 0) ui::setState("sleeping");
    send_status();
  } else if (!idle && dimmed) {
    dimmed = false;
    board::setBrightness(brightness);
    if (strcmp(ui::state(), "sleeping") == 0) ui::setState("idle");
    send_status();
  }
}

// Returns false if the server rejected the token (re-adopt), true for anything else (start over).
bool run_online() {
  String body;
  const int code = request("GET", "/api/v1/device/config", "", &body, true);
  if (code == 401) return false;
  if (code != 200) {
    LOGW(TAG, "config: HTTP %d", code);
    return true;
  }
  {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) apply_config(doc);
  }

  const String path = server.base + "/api/v1/device/ws";
  if (server.secure) ws.beginSslWithCA(server.host.c_str(), server.port, path.c_str(), CA_PEM, "");
  else ws.begin(server.host, server.port, path, "");
  ws.setExtraHeaders(("Authorization: Bearer " + token).c_str());
  ws.onEvent(on_ws_event);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(25000, 10000, 2);
  ws_connected = false;
  ws_down_since = millis();

  for (;;) {
    ws.loop();
    if (pending_config.length()) {
      JsonDocument doc;
      const String raw = pending_config;
      pending_config = "";
      // The socket always opens with the current config, which was usually just fetched over HTTP: skip that one
      // if nothing changed. Later pushes (including "Resend config") are always applied.
      if (!deserializeJson(doc, raw)) {
        const bool skip = first_ws_config && config_key(doc) == applied_key;
        if (!skip) apply_config(doc);
      }
      first_ws_config = false;
      send_status();
    }
    if (want_config && ws_connected) {
      want_config = false;
      ws.sendTXT("{\"type\":\"config_request\"}");
    }
    if (ws_connected && millis() - last_status > STATUS_INTERVAL_MS) send_status();
    if (millis() - last_log_flush > LOG_FLUSH_MS) flush_logs();
    update_voice();
    update_sleep();
    if (play_state_until && static_cast<int32_t>(millis() - play_state_until) > 0) {
      play_state_until = 0;
      ui::setState(play_state_restore.c_str());
      send_status();
    }
    if (WiFi.status() != WL_CONNECTED) {
      ws.disconnect();
      return true;
    }
    if (!ws_connected && ws_down_since && millis() - ws_down_since > WS_GIVE_UP_MS) {
      // The socket won't come back: check whether the token was revoked or the server is down.
      ws.disconnect();
      return request("GET", "/api/v1/device/config", "", nullptr, true) != 401;
    }
    delay(10);
  }
}

void toggle_mic() {
  const bool muted = !audio::micMuted();
  audio::setMicMuted(muted);
  store::setMicMuted(muted);
  ui::setMuted(muted);
  audio::play(muted ? audio::Sound::MuteOn : audio::Sound::MuteOff);
  LOGI(TAG, "mic %s", muted ? "muted" : "on");
  status_dirty = true;
}

// BOOT button: a short press mutes or unmutes the mic. Hold 5 s for Wi-Fi setup, 10 s for a factory reset.
void button_task(void*) {
  uint32_t down_at = 0;
  int stage = 0;
  for (;;) {
    if (board::buttonDown()) {
      if (!down_at) down_at = millis();
      const uint32_t held = millis() - down_at;
      if (held > 10000 && stage < 2) {
        stage = 2;
        ui::showMessage("Release to factory reset", "Forgets Wi-Fi and adoption", false);
      } else if (held > 5000 && stage < 1) {
        stage = 1;
        ui::showMessage("Release for Wi-Fi setup", "Keep holding to factory reset", false);
      }
    } else if (down_at) {
      if (stage == 1) {
        store::requestPortal();
        ESP.restart();
      } else if (stage == 2) {
        store::factoryReset();
        ESP.restart();
      }
      if (millis() - down_at < 1000) toggle_mic();
      board::markActivity();
      down_at = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void main_task(void*) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[13];
  snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  device_id = id;
  server_url = store::serverUrl();
  server = parse_url(server_url);
  token = store::token();
  LOGI(TAG, "Kotodama speaker %s, firmware %s, server %s, %s", device_id.c_str(), FW_VERSION, server_url.c_str(),
       token.length() ? "adopted" : "not adopted");

  volume = store::volume(70);
  if (audio::begin()) {
    audio::setVolume(volume);
    audio::setMicMuted(store::micMuted());
    audio::play(audio::Sound::Boot);
  }
  ui::setMuted(audio::micMuted());
  ui::onTouch([] { tap_requested = true; },
              [](int delta) {
                const int before = volume;
                set_volume_local(before + delta);
                if (volume / 5 != before / 5) audio::play(audio::Sound::Tick);
                return static_cast<int>(volume);
              });

  ui::showMessage("Starting", ap_name().c_str());
  board::setBrightness(brightness);
  if (store::takePortalRequest() || store::wifiNetworks().empty()) portal::run(ap_name());
  rebuild_wifi();

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) connect_wifi();
    if (!find_server()) continue;
    if (!token.length() && !adopt()) continue;
    if (!run_online()) {
      LOGW(TAG, "the server no longer accepts this speaker's token; asking for adoption again");
      token = "";
      store::clearToken();
    } else {
      delay(3000);
    }
  }
}

}  // namespace

void start() {
  xTaskCreatePinnedToCore(main_task, "kotodama", 20480, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(button_task, "button", 4096, nullptr, 1, nullptr, 1);
}

}  // namespace app
