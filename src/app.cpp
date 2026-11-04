#include "app.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <Update.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "audio.h"
#include "board.h"
#include "card.h"
#include "player.h"
#include "ca_certs.h"
#include "logbuf.h"
#include "relay.h"
#include "store.h"
#include "timers.h"
#include "ui.h"
#include "wakeword.h"
#include "web.h"

namespace app {
namespace {

constexpr const char* TAG = "app";
constexpr uint32_t STATUS_INTERVAL_MS = 30000;
constexpr uint32_t LOG_FLUSH_MS = 2000;
constexpr uint32_t WS_GIVE_UP_MS = 30000;  // then re-check the token over HTTP
constexpr size_t MAX_ASSET_BYTES = 3 * 1024 * 1024;
constexpr uint32_t MAX_LISTEN_MS = 30000;
constexpr uint32_t REPLY_TIMEOUT_MS = 60000;  // the first answer can be slow while Harkenn loads its models
constexpr uint32_t RING_EVERY_MS = 3000;  // how often the ring repeats while ringing
constexpr uint32_t CHIME_MS = 350;          // start listening after the activation chime, so it isn't sent
constexpr uint32_t VOLUME_SETTLE_MS = 1000;  // save and report the volume once the finger stops
constexpr uint32_t HOTSPOT_AFTER_MS = 60000;  // no saved network joined for this long: start the hotspot
constexpr uint32_t HOTSPOT_RETRY_MS = 60000;  // with the hotspot up, try the saved networks this often...
constexpr uint32_t HOTSPOT_QUIET_MS = 120000;  // ...but not while the page was used this recently (it drops clients)
constexpr uint32_t PHYSICAL_HOTSPOT_MS = 600000;  // Wi-Fi setup from the button: wait this long before retrying

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
uint32_t silence_ms = 5000;  // stop listening after this long without speech
bool dimmed = false;
String sleep_display = "character";  // character, clock or off (update_sleep)
std::map<String, std::shared_ptr<ui::Blob>> blobs;  // animations by URL, so unchanged ones aren't downloaded again
ui::CharacterSet shown;  // the animations on screen, kept for a state whose new download fails

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

// Through the relay (relay.cpp; docs/RELAY.md in server-docker): when Harkenn can't be reached directly, the same
// conversation goes, encrypted, through a relay both sides connect out to.
constexpr uint32_t RELAY_AFTER_MS = 120000;   // can't find Harkenn this long (with Wi-Fi): try the relay
constexpr uint32_t DIRECT_RETRY_MS = 300000;  // on the relay: try Harkenn directly this often
constexpr uint32_t FETCH_IDLE_MS = 20000;     // a fetch through the relay gives up after this long without data
constexpr uint32_t RELAY_GIVE_UP_MS = 90000;  // Harkenn looks for its speakers at the relay every 15 s
enum class RelayState { Off, Waiting, Confirming, Ready };
RelayState relay_state = RelayState::Off;
bool via_relay = false;  // this connection is through the relay
uint8_t* seal_buf = nullptr;
size_t seal_cap = 0;

// Sends a frame of the device protocol: as is, or sealed for the relay.
bool send_frame(char kind, const uint8_t* data, size_t len) {
  if (!via_relay) {
    return kind == 'T' ? ws.sendTXT(const_cast<uint8_t*>(data), len) : ws.sendBIN(data, len);
  }
  if (!relay::ready()) return false;
  if (seal_cap < len + relay::OVERHEAD) {
    void* grown = heap_caps_realloc(seal_buf, len + relay::OVERHEAD, MALLOC_CAP_SPIRAM);
    if (!grown) return false;
    seal_buf = static_cast<uint8_t*>(grown);
    seal_cap = len + relay::OVERHEAD;
  }
  const size_t n = relay::seal(kind, data, len, seal_buf);
  return n && ws.sendBIN(seal_buf, n);
}

bool send_text(const String& text) {
  return send_frame('T', reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

// A file through the relay, instead of downloading it: Harkenn answers fetch with fetch_result, then the data in
// 'F' frames, which go to the sink as they come (with the total size).
using FetchSink = std::function<bool(const uint8_t* data, size_t len, size_t total)>;
struct Fetch {
  uint32_t id = 0;
  int status = 0;  // 0 until fetch_result
  size_t size = 0, got = 0, max = 0;
  bool failed = false;
  FetchSink sink;
};
Fetch fetching;
uint32_t fetch_seq = 0;

void fetch_result(JsonDocument& doc) {
  if ((doc["id"] | 0u) != fetching.id) return;
  fetching.status = doc["status"] | 500;
  fetching.size = doc["size"] | 0;
  if (fetching.size > fetching.max) fetching.failed = true;
}

void fetch_data(const uint8_t* data, size_t len) {
  if (len < 4 || fetching.status != 200 || fetching.failed) return;
  const uint32_t id = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  if (id != fetching.id) return;
  if (fetching.got + len - 4 > fetching.size || !fetching.sink(data + 4, len - 4, fetching.size)) {
    fetching.failed = true;
    return;
  }
  fetching.got += len - 4;
}

// Returns the HTTP-like status (200 with all the data), or -1 if the fetch failed or timed out.
int relay_fetch(const String& url, size_t max, FetchSink sink) {
  if (!via_relay || !ws_connected) return -1;
  fetching = Fetch();
  fetching.id = ++fetch_seq;
  fetching.max = max;
  fetching.sink = std::move(sink);
  JsonDocument req;
  req["type"] = "fetch";
  req["id"] = fetching.id;
  req["url"] = url;
  String out;
  serializeJson(req, out);
  if (!send_text(out)) return -1;
  uint32_t last = millis();
  size_t seen = 0;
  int status_seen = 0;
  while (ws_connected && !fetching.failed && millis() - last < FETCH_IDLE_MS) {
    ws.loop();
    if (fetching.status && fetching.status != 200) return fetching.status;
    if (fetching.status == 200 && fetching.got == fetching.size) return 200;
    if (fetching.got != seen || fetching.status != status_seen) {  // progress
      seen = fetching.got;
      status_seen = fetching.status;
      last = millis();
    }
    delay(1);
  }
  LOGW(TAG, "fetch through the relay failed: %s", url.c_str());
  return -1;
}

// Touch, button and voice
volatile bool tap_requested = false;
volatile bool status_dirty = false;
volatile uint32_t volume_changed_at = 0;
uint32_t status_text_until = 0;
// Firmware update requested by Harkenn, run from the main loop
struct FirmwareUpdate {
  String url, version, sha256;
  size_t size = 0;
};
std::unique_ptr<FirmwareUpdate> pending_update;
std::unique_ptr<FirmwareUpdate> offered_update;  // shown on screen as "Update available"
volatile bool offer_install = false, offer_dismiss = false;
bool update_checked = false;  // asked Harkenn for updates since boot

enum class Voice { Off, Arbitrating, Starting, Listening, Waiting, Speaking };
bool wake_arbitration = true;      // config audio.wake_arbitration: only the nearest speaker answers (arbitrate())
bool audio_restart_wanted = false;  // it changed: restart, so the codec runs at the rate it needs
uint32_t arbitration_started = 0;
Voice voice = Voice::Off;
uint32_t voice_at = 0;
uint32_t voice_session = 0;
// The boot sound waits until the first config is fully in: every animation and sound downloaded, the boot sound too.
bool boot_sound_played = false;

void play_boot_sound() {
  if (boot_sound_played) return;
  boot_sound_played = true;
  audio::play(audio::Sound::Boot);
}

bool server_heard_speech = false;  // Harkenn ended listening (listen_stop): it heard speech
bool listen_after_speech = false;  // speak_start {listen}: someone asked a question; listen when it's been said
bool follow_up_next = false;       // ...and the reply continues the conversation (speak_start {follow_up})
bool follow_up_turn = false;       // this utterance is a reply in an ongoing conversation
String ring_id;                    // ringing for an agent (ring): tap to answer
uint32_t ring_until = 0, ring_next = 0;
size_t speech_dropped = 0;

// The assistant's screen (screen): downloaded and shown from the main loop, taps reported back (screen_tap).
struct PendingScreen {
  String id, url;
  std::shared_ptr<ui::Blob> image;  // sent on the socket after the screen message; else downloaded from url
  std::vector<ui::TapRegion> regions;
  bool tap_dismisses = true;
  uint32_t seconds = 60;
};
std::unique_ptr<PendingScreen> pending_screen;
std::unique_ptr<PendingScreen> receiving_screen;  // its picture is still arriving in binary frames
size_t receiving_got = 0;
String screen_id;
uint32_t screen_until = 0;
volatile bool screen_tapped = false;
char screen_tap_label[64];  // "" = dismissed

// The web page (web.cpp) runs in its own task: it asks for these, and the main loop does them.
volatile bool wifi_changed = false;        // saved networks changed: rebuild the list, try them now
volatile bool reconnect_wanted = false;    // the server address changed
volatile bool local_changes_due = true;    // send local changes to Harkenn (at every connection too)
volatile bool update_check_wanted = false;
volatile bool factory_reset_wanted = false;  // Harkenn asked
volatile bool card_rescan_wanted = false;  // Harkenn asked
volatile bool card_format_wanted = false;  // Harkenn asked (admin only: it erases the card)
uint32_t card_sent = UINT32_MAX;  // the card::version() Harkenn last heard about
uint32_t media_sent = UINT32_MAX;  // the player::version() Harkenn last heard about
char device_name[49] = "";
char offered_version[32] = "";  // the firmware Harkenn offered, for the page
bool static_ip_set = false;      // this connection uses a static IP from the saved network

String ap_name() {
  String suffix = device_id.substring(8);
  suffix.toUpperCase();
  return "Harkenn-" + suffix;
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

// Downloads firmware into the spare app slot and checks its SHA-256. Returns an error message, or "" on success
// (the slot is then the one to boot next).
// Writes an image into the spare app slot as it comes, checking its SHA-256 and showing progress.
struct ImageWriter {
  const FirmwareUpdate& fw;
  mbedtls_sha256_context sha;
  size_t done = 0;
  int shown = -1;
  String error;

  explicit ImageWriter(const FirmwareUpdate& f) : fw(f) {
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    if (!Update.begin(fw.size, U_FLASH)) error = String("can't start: ") + Update.errorString();
  }
  ~ImageWriter() { mbedtls_sha256_free(&sha); }

  bool write(const uint8_t* data, size_t n) {
    if (error.length()) return false;
    if (done + n > fw.size || Update.write(const_cast<uint8_t*>(data), n) != n) {
      error = String("flash write failed: ") + Update.errorString();
      return false;
    }
    mbedtls_sha256_update(&sha, data, n);
    done += n;
    const int pct = done * 100 / fw.size;
    if (pct / 2 != shown) {  // every 2%
      shown = pct / 2;
      char detail[48];
      snprintf(detail, sizeof(detail), "%s  %d%%", fw.version.c_str(), pct);
      ui::showProgress("Updating firmware", detail, pct);
    }
    return true;
  }

  // "" when the image is complete and checks out: it boots next.
  String finish(String problem) {
    if (!problem.length()) problem = error;
    if (!problem.length() && done != fw.size) problem = "download interrupted";
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (!problem.length() && fw.sha256 != hex) problem = "checksum mismatch";
    if (problem.length()) {
      Update.abort();
      return problem;
    }
    return Update.end(true) ? String() : String("can't finish: ") + Update.errorString();
  }
};

String install_firmware(const FirmwareUpdate& fw) {
  if (!fw.size || fw.sha256.length() != 64) return "bad update request";
  if (via_relay) {  // through the relay: Harkenn sends the image itself
    ImageWriter writer(fw);
    const int status = relay_fetch(fw.url, fw.size, [&writer](const uint8_t* data, size_t len, size_t) {
      return writer.write(data, len);
    });
    return writer.finish(status == 200 ? String() : "download failed: " + String(status));
  }
  auto client = make_client(fw.url);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  if (!http.begin(*client, fw.url)) return "bad URL";
  http.addHeader("Authorization", "Bearer " + token);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return "download failed: HTTP " + String(code);
  }
  if (http.getSize() != static_cast<int>(fw.size)) {
    http.end();
    return "unexpected size " + String(http.getSize());
  }
  ImageWriter writer(fw);
  if (writer.error.length()) {
    http.end();
    return writer.error;
  }
  static uint8_t buf[4096];
  WiFiClient* stream = http.getStreamPtr();
  uint32_t last_data = millis();
  String problem;
  while (writer.done < fw.size) {
    const int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected() || millis() - last_data > 15000) {
        problem = "download interrupted";
        break;
      }
      delay(2);
      continue;
    }
    const size_t n = stream->readBytes(buf, min(sizeof(buf), min(static_cast<size_t>(avail), fw.size - writer.done)));
    if (!writer.write(buf, n)) break;
    last_data = millis();
  }
  http.end();
  return writer.finish(problem);
}

std::shared_ptr<ui::Blob> download(const String& url) {
  if (via_relay) {  // through the relay: Harkenn sends it
    auto blob = std::make_shared<ui::Blob>();
    size_t at = 0;
    const int status = relay_fetch(url, MAX_ASSET_BYTES, [&blob, &at](const uint8_t* data, size_t len, size_t total) {
      if (!blob->data) {
        blob->data = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
        blob->size = total;
      }
      if (!blob->data || at + len > blob->size) return false;
      memcpy(blob->data + at, data, len);
      at += len;
      return true;
    });
    if (status == 200 && blob->data && at == blob->size) return blob;
    LOGW(TAG, "download %s through the relay: %d", url.c_str(), status);
    return nullptr;
  }
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

// The hotspot, for the web page (web.cpp): WPA2 with the speaker password's key when Harkenn has sent one, else
// open (a speaker that isn't adopted yet). `physical`: Wi-Fi setup opened with the button.
void start_hotspot(bool physical) {
  if (web::hotspotOn()) return;
  const String psk = store::localAccess().hotspot_psk;
  const bool secured = psk.length() == 64;
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(ap_name().c_str(), secured ? psk.c_str() : nullptr)) LOGE(TAG, "couldn't start the hotspot");
  web::setHotspot(true, physical);
  LOGI(TAG, "hotspot %s (%s) at %s", ap_name().c_str(), secured ? "speaker password" : "open",
       WiFi.softAPIP().toString().c_str());
  ui::showWifiSetup(physical ? "Wi-Fi setup" : "Can't join Wi-Fi", ap_name().c_str(), secured);
}

void stop_hotspot() {
  if (!web::hotspotOn()) return;
  web::setHotspot(false, false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  LOGI(TAG, "hotspot off");
}

// Static IP from the saved network we're on; DHCP when it has none.
void apply_static_ip() {
  const String ssid = WiFi.SSID();
  for (const auto& n : store::wifiNetworks()) {
    if (n.ssid != ssid || !n.ip.length()) continue;
    IPAddress ip, gateway, subnet, dns;
    if (!ip.fromString(n.ip) || !gateway.fromString(n.gateway) || !subnet.fromString(n.subnet)) return;
    if (!dns.fromString(n.dns)) dns = gateway;
    if (WiFi.localIP() == ip && static_ip_set) return;
    if (WiFi.config(ip, gateway, subnet, dns)) {
      static_ip_set = true;
      LOGI(TAG, "static IP %s on %s", n.ip.c_str(), ssid.c_str());
    } else {
      LOGE(TAG, "couldn't set static IP %s", n.ip.c_str());
    }
    return;
  }
  if (static_ip_set) {  // moved to a network without one
    WiFi.config(IPAddress(), IPAddress(), IPAddress());
    static_ip_set = false;
    LOGI(TAG, "DHCP on %s", ssid.c_str());
  }
}

// Joins a saved network. Without one for a minute (or with none saved, or `physical`), the hotspot comes up for the
// web page, and the saved networks are still tried every minute. Doesn't return until a network is joined.
void connect_wifi(bool physical) {
  WiFi.setHostname(("harkenn-" + device_id.substring(8)).c_str());
  if (physical || store::wifiNetworks().empty()) {
    start_hotspot(physical);
  } else {
    if (!web::hotspotOn()) WiFi.mode(WIFI_STA);
    ui::showMessage("Connecting to Wi-Fi", store::wifiNetworks().front().ssid.c_str());
  }
  const uint32_t started = millis();
  uint32_t next_try = physical ? started + PHYSICAL_HOTSPOT_MS : started;
  for (;;) {
    bool now = false;
    if (wifi_changed) {  // saved on the page: try it straight away
      wifi_changed = false;
      rebuild_wifi();
      now = true;
    }
    const bool due = static_cast<int32_t>(millis() - next_try) >= 0 &&
                     !(web::hotspotOn() && web::inUse(HOTSPOT_QUIET_MS));
    if (!store::wifiNetworks().empty() && (now || due)) {
      if (wifi->run(10000) == WL_CONNECTED) break;
      next_try = millis() + (web::hotspotOn() ? HOTSPOT_RETRY_MS : 500);
    }
    if (!web::hotspotOn() && millis() - started > HOTSPOT_AFTER_MS) start_hotspot(false);
    delay(200);
  }
  static_ip_set = false;
  apply_static_ip();
  LOGI(TAG, "Wi-Fi: %s, IP %s, RSSI %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  configTzTime(timezone.c_str(), "pool.ntp.org", "time.google.com");
  MDNS.begin(("harkenn-" + device_id.substring(8)).c_str());  // for finding the server
}

// Server discovery and adoption

// Where the server might be, in the order tried (docs/PROTOCOL.md, "Finding the server").
std::vector<String> server_candidates() {
  std::vector<String> out;
  auto add = [&out](const String& url) {
    if (url.length() && std::find(out.begin(), out.end(), url) == out.end()) out.push_back(url);
  };
  add(store::serverUrl());
  add(store::announcedServerUrl());
  const int found = MDNS.queryService("harkenn", "tcp");
  for (int i = 0; i < found; i++) {
    add(MDNS.txt(i, "url"));
    add("http://" + MDNS.address(i).toString() + ":" + String(MDNS.port(i)));
  }
  const IPAddress local = MDNS.queryHost("harkenn", 1500);
  if (local != IPAddress()) add("http://" + local.toString() + ":8000");
  add("http://harkenn");  // a DNS name you can point at the server when mDNS doesn't reach the speaker
  add("http://harkenn:8000");
  return out;
}

// Pings server_url; true if a Harkenn server answers. Sets the clock from its reply.
bool ping_server() {
  String body;
  const int code = request("GET", "/api/v1/ping", "", &body, false);
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, body)) return false;
  const String name = doc["server"] | "";
  if (name != "harkenn") return false;  // some other web server answering on that name
  const double t = doc["time"] | 0.0;
  if (time(nullptr) < 1700000000 && t > 1.7e9) {  // set the clock now rather than waiting for NTP
    timeval tv{static_cast<time_t>(t), 0};
    settimeofday(&tv, nullptr);
  }
  return true;
}

// Looks for Harkenn until it answers, Wi-Fi is lost, or `give_up_ms` have passed (0: never gives up).
bool find_server(uint32_t give_up_ms = 0) {
  ui::showMessage("Looking for Harkenn", "On this network");
  const uint32_t started = millis();
  while (WiFi.status() == WL_CONNECTED && !(give_up_ms && millis() - started > give_up_ms)) {
    for (const String& url : server_candidates()) {
      server_url = url;
      server = parse_url(url);
      if (ping_server()) {
        LOGI(TAG, "found Harkenn at %s", server_url.c_str());
        return true;
      }
    }
    LOGW(TAG, "no Harkenn server found yet");
    const String hint = web::hotspotOn() ? "Set its address on hotspot " + ap_name() + ", at 192.168.4.1"
                                         : "Set its address at http://" + WiFi.localIP().toString();
    ui::showMessage("Looking for Harkenn", hint.c_str(), true);
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
      store::setServerUrl(server_url);  // the server that adopted us, tried first from now on
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

// The sound pack's clips: download the ones that changed into flash. null = the speaker's own sounds.
constexpr const char* SOUND_SLOTS[] = {"boot", "wake", "done", "error", "shutdown"};

bool apply_sounds(JsonVariantConst sounds) {
  bool complete = true;
  for (const char* slot : SOUND_SLOTS) {
    JsonVariantConst s = sounds["slots"][slot];
    if (sounds.isNull() || s.isNull()) {
      if (audio::soundSource(slot).length()) audio::removeSound(slot);
      continue;
    }
    const String url = s["url"] | "";
    const String source = url.length() ? url : String("silent:") + (sounds["pack"] | "");
    if (audio::soundSource(slot) == source) continue;
    if (!url.length()) {
      audio::storeSound(slot, source, nullptr, 0);
      continue;
    }
    auto clip = download(url);
    if (!clip || !audio::storeSound(slot, source, clip->data, clip->size)) complete = false;
  }
  return complete;
}

// The wake word model (config "wake_word"): kept in flash like the sound clips; null = no wake word.
String wake_model_url;  // the model loaded now

bool apply_wake_word(JsonVariantConst w) {
  const String url = w["url"] | "";
  if (w.isNull() || !url.length()) {
    if (wakeword::loaded()) LOGI(TAG, "wake word off");
    wakeword::unload();
    audio::removeSound("wakeword");
    wake_model_url = "";
    return true;
  }
  if (url == wake_model_url && wakeword::loaded()) return true;
  size_t size = 0;
  uint8_t* data = nullptr;
  std::shared_ptr<ui::Blob> blob;
  if (audio::soundSource("wakeword") == url) data = audio::readSound("wakeword", &size);
  if (!data) {
    blob = download(url);
    if (!blob) return false;
    audio::storeSound("wakeword", url, blob->data, blob->size);
  }
  const bool ok = wakeword::load(data ? data : blob->data, data ? size : blob->size, w["cutoff"] | 0.97f,
                                 w["window"] | 5, w["arena"] | 30000);
  free(data);
  if (ok) {
    wake_model_url = url;
    LOGI(TAG, "wake word: %s", (const char*)(w["name"] | "?"));
  }
  return ok;
}

// "Stop" (config "stop_word"): a second model, listening only while a timer or alarm rings. Kept in flash too.
String stop_model_url;

bool apply_stop_word(JsonVariantConst w) {
  const String url = w["url"] | "";
  if (w.isNull() || !url.length()) {
    wakeword::unloadStop();
    stop_model_url = "";
    return true;
  }
  if (url == stop_model_url && wakeword::stopLoaded()) return true;
  size_t size = 0;
  uint8_t* data = nullptr;
  std::shared_ptr<ui::Blob> blob;
  if (audio::soundSource("stopword") == url) data = audio::readSound("stopword", &size);
  if (!data) {
    blob = download(url);
    if (!blob) return false;
    audio::storeSound("stopword", url, blob->data, blob->size);
  }
  const bool ok = wakeword::loadStop(data ? data : blob->data, data ? size : blob->size, w["cutoff"] | 0.5f,
                                     w["window"] | 5, w["arena"] | 21000);
  free(data);
  if (ok) stop_model_url = url;
  return ok;
}

bool has_ssid(JsonVariantConst list, const String& ssid) {
  for (JsonVariantConst v : list.as<JsonArrayConst>()) {
    if ((v.is<JsonObjectConst>() ? String(v["ssid"] | "") : String(v | "")) == ssid) return true;
  }
  return false;
}

// What the web page needs from the config: password hashes, the hotspot key, the unlock counter.
void apply_local_access(JsonVariantConst la) {
  if (la.isNull()) return;  // an older Harkenn: keep what we have
  store::LocalAccess next;
  next.admin_hash = la["admin_hash"] | "";
  next.speaker_hash = la["speaker_hash"] | "";
  next.hotspot_psk = la["hotspot_psk"] | "";
  next.unlock = la["unlock"] | 0;
  const store::LocalAccess old = store::localAccess();
  if (next.unlock != old.unlock && (web::failures() || web::locked())) web::unlock();
  if (next.admin_hash != old.admin_hash || next.speaker_hash != old.speaker_hash ||
      next.hotspot_psk != old.hotspot_psk || next.unlock != old.unlock) {
    store::setLocalAccess(next);
    LOGI(TAG, "web page passwords updated");
  }
}

// Remote connection through the relay (config "remote"): its address and our credentials; null = not set up.
void apply_remote(JsonVariantConst r) {
  store::Remote next;
  if (!r.isNull()) {
    next.url = r["url"] | "";
    next.id = r["id"] | "";
    next.key = r["key"] | "";
  }
  const store::Remote old = store::remote();
  if (next.url == old.url && next.id == old.id && next.key == old.key) return;
  store::setRemote(next);
  LOGI(TAG, next.url.length() ? "remote connection through the relay: on" : "remote connection: off");
}

void apply_config(JsonDocument& doc) {
  JsonObject cfg = doc["config"];
  JsonObject display = cfg["display"];
  // Changes made on the web page that Harkenn hasn't confirmed yet win over the config.
  JsonDocument local;
  if (deserializeJson(local, store::localChanges())) local.to<JsonObject>();
  strlcpy(device_name, doc["device"]["name"] | "", sizeof(device_name));
  apply_local_access(doc["local_access"]);
  apply_remote(doc["remote"]);

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

  bool complete = apply_sounds(doc["sounds"]);
  complete &= apply_wake_word(doc["wake_word"]);
  complete &= apply_stop_word(doc["stop_word"]);

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
        if (!blob) {
          delay(500);
          blob = download(url);  // one retry: a download can fail while the live connection is still starting
        }
        if (!blob) {
          set[i] = shown[i];  // keep what was there; the next config tries again
          complete = false;
          continue;
        }
        have = next.emplace(url, blob).first;
      }
      set[i] = have->second;
    }
  }
  blobs = next;

  brightness = local["brightness"].is<int>() ? local["brightness"].as<int>() : (display["brightness"] | 75);
  sleep_after_ms = static_cast<uint32_t>(display["sleep_after_s"] | 300) * 1000;
  sleep_display = display["sleep_display"] | "character";
  ui::showSleepClock(false);  // the config wakes the screen (below)
  silence_ms = constrain(cfg["audio"]["silence_s"] | 5, 1, 30) * 1000;
  wake_arbitration = cfg["audio"]["wake_arbitration"] | true;
  if (wake_arbitration != store::ultrasonic()) {  // the codec's rate is set at startup
    store::setUltrasonic(wake_arbitration);
    audio_restart_wanted = true;
  }
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
  const String level = local["log_level"] | (cfg["log_level"] | "info");
  logbuf::setUploadLevel(level == "error" ? 'E' : level == "warn" ? 'W' : level == "debug" ? 'D' : 'I');

  // Wi-Fi networks from the server are added to the saved list (or updated); only the web page removes any.
  bool networks_changed = false;
  for (JsonObject n : cfg["wifi"].as<JsonArray>()) {
    store::WifiNetwork w;
    w.ssid = n["ssid"] | "";
    if (!w.ssid.length() || has_ssid(local["wifi_removed"], w.ssid) || has_ssid(local["wifi"], w.ssid)) continue;
    w.password = n["password"] | "";
    w.ip = n["ip"] | "";
    w.subnet = n["subnet"] | "";
    w.gateway = n["gateway"] | "";
    w.dns = n["dns"] | "";
    networks_changed |= store::upsertWifi(w);
  }
  if (networks_changed) {
    rebuild_wifi();
    apply_static_ip();
    LOGI(TAG, "Wi-Fi list updated: %u networks", store::wifiNetworks().size());
  }

  shown = set;
  ui::setCharacter(set);
  ui::showLayout(theme, widgets, display["clock_24h"] | true);
  if (complete) play_boot_sound();  // else when a retry has everything
  dimmed = false;
  board::markActivity();
  board::setBrightness(brightness);
  config_version = doc["version"] | 0;
  // The server's public address: remembered as a second way to reach it (the adoption address stays first).
  const String announced = doc["server_url"] | "";
  if ((announced.startsWith("http://") || announced.startsWith("https://")) &&
      announced != store::announcedServerUrl()) {
    store::setAnnouncedServerUrl(announced);
    LOGI(TAG, "server's public address: %s", announced.c_str());
  }
  applied_key = complete ? config_key(doc) : String();  // incomplete: don't skip the same config next time
  LOGI(TAG, "applied config v%d (layout %s, character %s)", config_version, (const char*)(doc["layout"]["id"] | "?"),
       (const char*)(doc["character"]["id"] | "?"));
}

// Live connection

void send_json(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  send_text(out);
}

// Internal RAM (free now, largest block, lowest since boot) and each task's stack never used, for sizing them.
void add_memory(JsonObject s) {
  s["free_heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s["largest_free"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s["min_free_heap"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s["free_psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  JsonObject stacks = s["stack_free"].to<JsonObject>();
  for (const char* name : {"harkenn", "loopTask", "web", "wakeword", "audio_out", "audio_in", "button"}) {
    if (TaskHandle_t t = xTaskGetHandle(name)) stacks[name] = uxTaskGetStackHighWaterMark(t);
  }
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
  add_memory(s);
  s["battery"] = board::batteryPercent();
  s["charging"] = board::charging();
  s["volume"] = volume;
  s["muted"] = audio::micMuted();
  s["brightness"] = brightness;
  s["config_version"] = config_version;
  s["local_failures"] = web::failures();
  const store::Remote remote = store::remote();
  if (remote.url.length()) s["remote"] = remote.id;  // Harkenn meets us at the relay once it knows we have it
  s["via_relay"] = via_relay;
  s["local_locked"] = web::locked();
  s["hotspot"] = web::hotspotOn();
  card::addStatus(s);
  send_json(doc);
}

// What the player is doing (player.h), whenever it changes.
void send_media() {
  media_sent = player::version();
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "media_state";
  player::toJson(doc["media"].to<JsonObject>());
  send_json(doc);
}

// Harkenn's media commands (docs/PROTOCOL.md, "The media player").
void handle_media(JsonDocument& doc) {
  const String action = doc["action"] | "";
  if (action == "play") {
    player::Kind kind;
    if (!player::parseKind(doc["kind"] | "music", &kind)) return;
    player::play(kind, doc["album"] | "", doc["track"] | 0, doc["shuffle"] | false);
    if (doc["repeat"].is<const char*>()) {
      player::Repeat r;
      if (player::parseRepeat(doc["repeat"] | "", &r)) player::setRepeat(r);
    }
    if (doc["minutes"].is<int>()) player::sleepAfter(doc["minutes"] | 0);
  } else if (action == "pause") {
    player::pause();
  } else if (action == "resume") {
    player::resume();
  } else if (action == "stop") {
    player::stop();
  } else if (action == "next") {
    player::next();
  } else if (action == "previous") {
    player::previous();
  } else if (action == "repeat") {
    player::Repeat r;
    if (player::parseRepeat(doc["mode"] | "", &r)) player::setRepeat(r);
  } else if (action == "sleep") {
    player::sleepAfter(doc["minutes"] | 0);
  }
}

// What's on the SD card (or that there isn't one), whenever it changes and on connecting.
void send_card() {
  card_sent = card::version();
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "card";
  card::catalogue(doc["card"].to<JsonObject>());
  send_json(doc);
}

// Settings changed on the web page, until Harkenn acknowledges them (local_changes_ack).
void send_local_changes() {
  local_changes_due = false;
  const String changes = store::localChanges();
  if (!ws_connected || !changes.length()) return;
  send_text("{\"type\":\"local_changes\",\"id\":" + String(store::localChangesId()) + ",\"changes\":" + changes + "}");
  LOGI(TAG, "sent the changes made on the web page");
}

// Tells Harkenn about settings changed on the speaker, so its stored config matches.
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

// Voice: a tap starts listening. What the mic hears streams to Harkenn, which answers with speech (docs/PROTOCOL.md).

// Plays the shutdown sound and waits for it to finish (at most about 6 s).
void say_goodbye() {
  audio::play(audio::Sound::Shutdown);
  delay(50);
  const uint32_t start = millis();
  while (audio::playing() && millis() - start < 6000) delay(20);
  delay(250);  // let the last of it drain out of the DMA buffers
}

void restart() {
  say_goodbye();
  ESP.restart();
}

// Power off: deep sleep until the button is pressed, which starts the speaker up again.
void power_off() {
  LOGI(TAG, "powering off");
  ui::showMessage("Goodbye!", "Press the button to turn on", false);
  say_goodbye();
  board::setBrightness(0);
  while (board::buttonDown()) delay(20);  // released first, or the same press would wake it
  delay(100);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

void flash_state(const char* state, uint32_t ms) {
  if (!play_state_until) play_state_restore = "idle";
  ui::setState(state);
  play_state_until = millis() + ms;
}

void show_status_text(const char* text, uint32_t ms) {
  ui::setStatusText(text);
  status_text_until = millis() + ms;
}

void send_voice(const char* type) {
  JsonDocument doc;
  doc["type"] = type;
  doc["session"] = String(voice_session);
  send_json(doc);
}

void voice_done(const char* state = "idle") {
  voice = Voice::Off;
  ui::setState(state);
}

void voice_failed(const char* why) {
  LOGW(TAG, "voice: %s", why);
  voice = Voice::Off;
  audio::play(audio::Sound::Error);
  flash_state("error", 3000);
}

void start_voice() {
  char now[24];
  const time_t t = time(nullptr);
  struct tm local;
  localtime_r(&t, &local);
  strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%S", &local);
  voice_session++;
  JsonDocument doc;
  doc["type"] = "voice_start";
  doc["session"] = String(voice_session);
  doc["rate"] = audio::SAMPLE_RATE;
  if (t > 1700000000) doc["local_time"] = now;
  if (follow_up_turn) doc["follow_up"] = true;
  send_json(doc);
  voice = Voice::Listening;
  server_heard_speech = false;
}

// Sends what the mic has queued; when listening has stopped, ends the utterance.
void pump_mic() {
  static uint8_t buf[3200];  // 100 ms
  size_t n;
  while ((n = audio::readMic(buf, sizeof(buf))) > 0) send_frame('B', buf, n);
  if (audio::listening()) return;
  const audio::ListenInfo info = audio::listenInfo();
  if (audio::micMuted()) {  // muted mid-sentence: drop it
    send_voice("voice_cancel");
    voice_done();
  } else if (!info.speech && !server_heard_speech) {
    LOGI(TAG, "heard nothing in %u ms (noise floor %d, peak %d%%)", info.duration_ms, info.noise_floor,
         info.peak_percent);
    send_voice("voice_cancel");
    if (!follow_up_turn) audio::play(audio::Sound::Cancel);  // no reply: the conversation just ends, quietly
    voice_done();
  } else {
    LOGI(TAG, "sent %u ms of speech (peak %d%%, noise floor %d, %u ms dropped)", info.duration_ms, info.peak_percent,
         info.noise_floor, info.dropped_ms);
    send_voice("voice_end");
    audio::play(audio::Sound::Done);
    ui::setState("thinking");
    voice = Voice::Waiting;
    voice_at = millis() + REPLY_TIMEOUT_MS;
  }
}

void stop_ringing() {
  ring_id = "";
  ui::setStatusText("");
  status_text_until = 0;
}

void start_ringing(const String& id, uint32_t seconds) {
  if (voice != Voice::Off) return;  // in a conversation already: not answered, Harkenn tells the agent
  ring_id = id;
  ring_until = millis() + constrain(seconds, 5u, 30u) * 1000;
  ring_next = millis();
  board::markActivity();  // wake the screen
  ui::setState("ringing");  // the character's ringing animation (idle if it has none)
  ui::setStatusText("Tap to answer");
  status_text_until = 0;
  LOGI(TAG, "ringing for %u s", seconds);
}

// A new conversation (tap, wake word) plays the wake sound; listening for a reply after speaking plays a soft cue.
// `queued_ms`: sound already queued before the cue (the claim tone), so listening starts after both.
void start_listening_turn(bool after_speech = false, bool follow_up = false, uint32_t queued_ms = 0) {
  const audio::Sound cue = after_speech ? audio::Sound::Listen : audio::Sound::Activate;
  audio::play(cue);
  follow_up_turn = follow_up;
  ui::setState("listening");
  voice = Voice::Starting;
  // start listening once the cue has played (a voiced wake sound is longer than a chime), so it isn't sent
  voice_at = millis() + queued_ms + max(CHIME_MS, audio::soundMs(cue) + 120);
}

// Wake word arbitration: when several speakers hear the wake word, only the one that heard it loudest answers.
// Each turns how loud it heard the word into a level (0 the loudest, 2 dB a level), waits longer the higher its
// level, then plays its claim (three tones above hearing that carry the level; audio.h) and answers. Strongest
// wins:
// - before claiming, a speaker that hears a claim as strong as its own, or stronger, stays quiet;
// - one that hears a weaker claim (that speaker caught the word sooner) claims at once;
// - after claiming, a speaker that hears a stronger claim within CHALLENGE_MS stops answering and goes back to idle.
constexpr int ARBITRATION_LOUD_DB = -10;  // heard this loud or louder: level 0
constexpr int ARBITRATION_DB_PER_LEVEL = 2;
constexpr uint32_t ARBITRATION_MS_PER_LEVEL = 25;
constexpr uint32_t ARBITRATION_JITTER_MS = 20;  // two speakers at the same level rarely claim together
constexpr uint32_t CLAIM_LOOKBACK_MS = 1500;    // a claim heard this long before we caught the word still counts
constexpr uint32_t CHALLENGE_MS = 1500;         // after our claim, a stronger one still takes over
int my_level = 0;
uint32_t claimed_at = 0;  // when we claimed; 0 = no claim of ours in play

int level_for(int db) {
  return constrain((ARBITRATION_LOUD_DB - db + ARBITRATION_DB_PER_LEVEL - 1) / ARBITRATION_DB_PER_LEVEL, 0,
                   audio::CLAIM_LEVELS - 1);
}

void arbitrate() {
  const int db = audio::recentLevelDb();
  my_level = level_for(db);
  claimed_at = 0;
  uint32_t wait = my_level * ARBITRATION_MS_PER_LEVEL + esp_random() % ARBITRATION_JITTER_MS;
  const audio::HeardClaim heard = audio::lastClaim();
  if (heard.at && millis() - heard.at < CLAIM_LOOKBACK_MS) {
    if (heard.level <= my_level) {
      LOGI(TAG, "wake word at %d dBFS (level %d): another speaker heard it at level %d and is answering; staying quiet",
           db, my_level, heard.level);
      return;
    }
    wait = 0;  // we heard it better than the speaker answering: take over
  }
  LOGI(TAG, "wake word at %d dBFS (level %d): answering in %u ms unless a closer speaker does", db, my_level,
       static_cast<unsigned>(wait));
  arbitration_started = millis();
  voice = Voice::Arbitrating;
  voice_at = millis() + wait;
}

// Another speaker heard the wake word better: stop whatever we started, quietly.
void stand_down(const audio::HeardClaim& other) {
  LOGI(TAG, "wake word: another speaker heard it better (level %d, ours %d, its signal %d here); standing down",
       other.level, my_level, other.strength);
  if (voice == Voice::Listening) {
    audio::stopListening();
    send_voice("voice_cancel");
  } else if (voice == Voice::Waiting) {
    send_voice("voice_cancel");
  } else if (voice == Voice::Speaking) {
    audio::stopSpeech();
    send_voice("speak_stop");
  }
  claimed_at = 0;
  listen_after_speech = false;
  voice_done();
}

void update_arbitration() {
  const audio::HeardClaim heard = audio::lastClaim();
  if (voice == Voice::Arbitrating) {
    if (heard.at && static_cast<int32_t>(heard.at - arbitration_started) >= 0) {
      if (heard.level <= my_level) {  // as strong or stronger, and first
        stand_down(heard);
        return;
      }
      voice_at = millis();  // weaker: claim now, so it stands down
    }
    if (static_cast<int32_t>(millis() - voice_at) >= 0) {
      audio::claim(my_level);  // tell the others, then answer
      claimed_at = millis() | 1;
      board::markActivity();
      start_listening_turn(false, false, audio::claimMs(my_level));
    }
  } else if (claimed_at) {
    if (voice == Voice::Off || millis() - claimed_at > CHALLENGE_MS) {
      claimed_at = 0;
    } else if (heard.at && static_cast<int32_t>(heard.at - claimed_at) > 0 && heard.level < my_level) {
      stand_down(heard);  // a stronger claim after ours
    }
  }
}

void handle_tap() {
  if (ring_id.length()) {  // answer
    JsonDocument doc;
    doc["type"] = "ring_answer";
    doc["ring"] = ring_id;
    send_json(doc);
    LOGI(TAG, "answered a ring");
    stop_ringing();  // no chime: the caller's message follows straight away
    ui::setState("thinking");  // until Harkenn speaks, or the ring ends
    return;
  }
  if (voice == Voice::Listening) {
    audio::stopListening();  // pump_mic() ends the utterance
  } else if (voice == Voice::Starting || voice == Voice::Waiting) {
    if (voice == Voice::Waiting) send_voice("voice_cancel");
    audio::play(audio::Sound::Cancel);
    voice_done();
  } else if (voice == Voice::Speaking) {
    audio::stopSpeech();
    send_voice("speak_stop");
    listen_after_speech = false;
    voice_done();
  } else if (audio::micMuted()) {
    audio::play(audio::Sound::Cancel);
    show_status_text("Mic is off", 2500);
  } else if (!ws_connected) {
    audio::play(audio::Sound::Error);
    show_status_text("Not connected", 2500);
  } else if (strcmp(ui::state(), "idle") == 0 || strcmp(ui::state(), "sleeping") == 0) {
    start_listening_turn();
  }
}

// Messages from Harkenn about the current utterance.
void handle_voice_message(const String& type, JsonDocument& doc) {
  if (type == "ring") {
    start_ringing(doc["ring"] | "", doc["seconds"] | 30);
    return;
  }
  if (type == "ring_stop") {
    if (ring_id.length() && ring_id == String(doc["ring"] | "")) {  // time's up, or answered on another speaker
      stop_ringing();
      voice_done();
    } else if (!ring_id.length() && voice == Voice::Off && strcmp(ui::state(), "thinking") == 0) {
      voice_done();  // answered here; nothing more to say
    }
    return;
  }
  if (type == "speak_start") {  // a reply, or something Harkenn wants said (admin "Say it", an agent)
    if (ring_id.length()) stop_ringing();
    listen_after_speech = doc["listen"] | false;
    follow_up_next = doc["follow_up"] | false;
    audio::startSpeech();
    speech_dropped = 0;
    voice = Voice::Speaking;
    ui::setState("speaking");
    return;
  }
  if (type == "speak_end") {
    audio::endSpeech();
    if (speech_dropped) LOGW(TAG, "speech: %u bytes didn't fit the buffer", speech_dropped);
    return;
  }
  if (String(doc["session"] | "") != String(voice_session)) return;  // about an utterance we've given up on
  if (type == "voice_wait" && voice == Voice::Waiting) {  // the answer is taking a while (an agent using tools)
    voice_at = millis() + REPLY_TIMEOUT_MS;
  } else if (type == "listen_stop" && voice == Voice::Listening) {  // Harkenn heard speech, then silence
    server_heard_speech = true;
    audio::stopListening();  // pump_mic() ends the utterance
  } else if (type == "voice_result") {
    const String heard = doc["heard"] | "";
    const String reply = doc["reply"] | "";
    if (!heard.length() && voice == Voice::Waiting) {
      if (!follow_up_turn) audio::play(audio::Sound::Cancel);
      voice_done();
    } else if (!reply.length() && voice == Voice::Waiting) {  // nothing to say: only a screen
      LOGI(TAG, "heard \"%s\"", heard.c_str());
      voice_done();
    } else {
      LOGI(TAG, "heard \"%s\"", heard.c_str());
    }
  } else if (type == "voice_error" && (voice == Voice::Waiting || voice == Voice::Listening)) {
    voice_failed((doc["message"] | "Harkenn couldn't answer"));
  }
}
// A labelled part of the assistant's screen was tapped: it's the person's next turn, instead of whatever was going on.
void tap_screen(const String& label) {
  if (voice == Voice::Listening) {
    audio::stopListening();
    send_voice("voice_cancel");
  } else if (voice == Voice::Starting) {
    audio::stopListening();
  } else if (voice == Voice::Waiting) {
    send_voice("voice_cancel");
  } else if (voice == Voice::Speaking) {
    audio::stopSpeech();
    send_voice("speak_stop");
  }
  listen_after_speech = false;
  voice_session++;
  JsonDocument doc;
  doc["type"] = "screen_tap";
  doc["session"] = String(voice_session);
  doc["screen"] = screen_id;
  doc["label"] = label;
  send_json(doc);
  LOGI(TAG, "screen: tapped \"%s\"", label.c_str());
  audio::play(audio::Sound::Done);
  ui::setState("thinking");
  voice = Voice::Waiting;
  voice_at = millis() + REPLY_TIMEOUT_MS;
}

void update_screen() {
  if (pending_screen) {
    auto sc = std::move(pending_screen);
    auto image = sc->image ? sc->image : download(sc->url);
    if (!image) {
      LOGW(TAG, "screen: couldn't download it");
    } else {
      ui::showScreen(image, sc->regions, sc->tap_dismisses);
      screen_id = sc->id;
      screen_until = millis() + sc->seconds * 1000;
      board::markActivity();  // wake the display
      LOGI(TAG, "screen: shown for %u s, %u tappable", sc->seconds, static_cast<unsigned>(sc->regions.size()));
    }
  }
  if (screen_tapped) {
    screen_tapped = false;
    const String label = screen_tap_label;
    ui::hideScreen();
    if (ring_id.length()) {
      handle_tap();  // ringing: any tap answers
    } else if (label.length()) {
      tap_screen(label);
    } else {
      JsonDocument doc;
      doc["type"] = "screen_dismiss";
      doc["screen"] = screen_id;
      send_json(doc);
    }
  }
  if (!ui::screenShown()) return;
  if (static_cast<int32_t>(millis() - screen_until) >= 0) ui::hideScreen();
  else board::markActivity();  // the display stays on while it's shown
}

// Timers and alarms (timers.cpp) ring here, and notifications from Harkenn show here; both keep an icon in the
// status row until they're dealt with. Ringing stops with a tap, the wake word, "stop" (its own small model,
// listening only now), Harkenn, or after TIMER_RING_MS / ALARM_RING_MS.
constexpr uint32_t TIMER_RING_MS = 5 * 60000;
constexpr uint32_t ALARM_RING_MS = 15 * 60000;
constexpr uint32_t RING_SOUND_EVERY_MS = 2500;  // the gaps let "stop" and the wake word be heard
constexpr uint32_t TIMER_SNOOZE_MIN = 1, ALARM_SNOOZE_MIN = 9;
constexpr size_t MAX_NOTES = 5;

struct TimerRing {
  bool on = false;
  timers::Item item;
  uint32_t started = 0, next_sound = 0;
};
TimerRing timer_ring;
volatile bool ring_stop_tapped = false, ring_snooze_tapped = false;
uint32_t timers_sent = UINT32_MAX;  // the timers::version() Harkenn last heard about
struct Note {
  String title, text;
};
std::vector<Note> notes;
volatile bool note_reopen = false, note_seen = false;

bool timer_ringing() { return timer_ring.on; }

void send_timers() {
  timers_sent = timers::version();
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "timers";
  timers::toJson(doc["items"].to<JsonArray>());
  if (timer_ring.on) doc["ringing"] = timer_ring.item.id;
  send_json(doc);
}

// For Harkenn's log, and so the assistant knows (rang, stopped, snoozed, missed).
void timer_event(const char* name, const timers::Item& item, const char* how) {
  if (!ws_connected) return;
  JsonDocument doc;
  doc["type"] = "event";
  doc["name"] = name;
  JsonObject d = doc["data"].to<JsonObject>();
  d["kind"] = item.alarm ? "alarm" : "timer";
  d["id"] = item.id;
  d["label"] = item.label;
  if (how && how[0]) d["how"] = how;
  send_json(doc);
}

String left_text(long left) {
  char buf[16];
  if (left < 0) left = 0;
  if (left >= 3600) snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", left / 3600, left / 60 % 60, left % 60);
  else snprintf(buf, sizeof(buf), "%ld:%02ld", left / 60, left % 60);
  return buf;
}

void start_timer_ring(const timers::Item& it) {
  timer_ring = TimerRing{true, it, millis(), millis()};
  board::markActivity();
  ui::hideCountdown();
  const String title = it.label.length() ? it.label : String(it.alarm ? "Alarm" : "Timer");
  const String detail = it.alarm ? timers::describe(it) : String(it.label.length() ? "Timer done" : "Time's up");
  ui::showRinging(title.c_str(), detail.c_str(), it.alarm ? "Snooze" : "+1 min", [] { ring_stop_tapped = true; },
                  [] { ring_snooze_tapped = true; });
  LOGI(TAG, "%s \"%s\" ringing", it.alarm ? "alarm" : "timer", it.label.c_str());
  timer_event("timer_ringing", it, "");
  timers_sent = UINT32_MAX;
}

void stop_timer_ring(const char* event, const char* how, bool snooze) {
  if (!timer_ring.on) return;
  const timers::Item it = timer_ring.item;
  timer_ring.on = false;
  ui::hideRinging();
  if (snooze) timers::snooze(it, it.alarm ? ALARM_SNOOZE_MIN : TIMER_SNOOZE_MIN);
  LOGI(TAG, "%s \"%s\": %s (%s)", it.alarm ? "alarm" : "timer", it.label.c_str(), event, how);
  timer_event(event, it, how);
  timers_sent = UINT32_MAX;
}

void refresh_status_row() {
  bool alarm = false;
  String timer;
  const time_t now = time(nullptr);
  for (const auto& i : timers::list()) {  // soonest first
    if (i.alarm) alarm = true;
    else if (!timer.length()) timer = left_text(static_cast<long>(i.due - now));
  }
  ui::setStatusIcons(via_relay, alarm, !notes.empty(), timer.c_str());
}

void clear_notifications(bool tell) {
  if (notes.empty()) return;
  notes.clear();
  if (tell && ws_connected) send_text("{\"type\":\"notifications_seen\"}");
}

void handle_notify(JsonDocument& doc) {
  Note n{doc["title"] | "", doc["text"] | ""};
  if (!n.text.length() && !n.title.length()) return;
  notes.push_back(n);
  if (notes.size() > MAX_NOTES) notes.erase(notes.begin());
  if (doc["quiet"] | false) return;  // the icon only (e.g. sent again after a restart)
  board::markActivity();
  if (voice == Voice::Off && !timer_ring.on) audio::play(audio::Sound::Notify);
  ui::showNotification(n.title.c_str(), n.text.c_str(), doc["seconds"] | 12, [] { note_seen = true; });
}

// Days of the week as a mask (bit 0 Sunday): a number, "daily", "weekdays", "weekends", or names ("mon,wed").
uint8_t parse_days(JsonVariantConst v) {
  if (v.is<int>()) return v.as<int>() & 0x7f;
  String s;
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst d : v.as<JsonArrayConst>()) s += String(d | "") + ",";
  } else {
    s = v | "";
  }
  s.toLowerCase();
  if (s.indexOf("daily") >= 0 || s.indexOf("every") >= 0) return 0x7f;
  if (s.indexOf("weekday") >= 0) return 0x3e;
  if (s.indexOf("weekend") >= 0) return 0x41;
  const char* names[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  uint8_t mask = 0;
  for (int d = 0; d < 7; d++) {
    if (s.indexOf(names[d]) >= 0) mask |= 1 << d;
  }
  return mask;
}

void show_timers() {
  const auto all = timers::list();
  for (const auto& i : all) {
    if (!i.alarm) {
      ui::showCountdown(i.label.c_str(), i.due, i.duration, 20);
      return;
    }
  }
  String text;
  for (const auto& i : all) text += (text.length() ? "\n" : "") + (i.label.length() ? i.label + ": " : "") +
                                    timers::describe(i);
  ui::showNotification(all.empty() ? "No timers" : "Alarms", text.length() ? text.c_str() : "Nothing is set", 8,
                       nullptr);
}

void handle_timer(JsonDocument& doc) {
  const String action = doc["action"] | "";
  const String label = doc["label"] | "";
  if (action == "set") {
    if (String(doc["kind"] | "timer") == "alarm") {
      const String t = doc["time"] | "";
      const int colon = t.indexOf(':');
      const uint32_t id = colon > 0 ? timers::addAlarm(label, t.substring(0, colon).toInt(),
                                                       t.substring(colon + 1).toInt(), parse_days(doc["days"]),
                                                       doc["date"] | "")
                                    : 0;
      if (!id) {
        LOGW(TAG, "couldn't set the alarm for \"%s\"", t.c_str());
      } else if (doc["show"] | true) {
        for (const auto& i : timers::list()) {
          if (i.id == id) ui::showNotification(label.length() ? label.c_str() : "Alarm set", timers::describe(i).c_str(),
                                                5, nullptr);
        }
      }
    } else {
      const uint32_t seconds = doc["seconds"] | 0;
      const uint32_t id = timers::addTimer(label, seconds);
      if (!id) LOGW(TAG, "couldn't set the timer (%u s)", static_cast<unsigned>(seconds));
      else if (doc["show"] | true) ui::showCountdown(label.c_str(), time(nullptr) + seconds, seconds, 12);
    }
  } else if (action == "cancel") {
    const String what = doc["what"].is<const char*>() ? String(doc["what"].as<const char*>())
                                                      : String(doc["what"] | 0);
    String ringing = timer_ring.item.label;
    ringing.toLowerCase();
    String w = what;
    w.toLowerCase();
    if (timer_ring.on && (w == "all" || w == ringing || w == String(timer_ring.item.id) ||
                          (w == "timers" && !timer_ring.item.alarm) || (w == "alarms" && timer_ring.item.alarm))) {
      stop_timer_ring("timer_stopped", "cancelled", false);
    }
    timers::cancelMatching(what);
    ui::hideCountdown();
  } else if (action == "show") {
    board::markActivity();
    show_timers();
  } else if (action == "stop") {
    stop_timer_ring("timer_stopped", "Harkenn", false);
  }
}

void update_timers() {
  if (ring_stop_tapped) {
    ring_stop_tapped = false;
    stop_timer_ring("timer_stopped", "a tap", false);
  }
  if (ring_snooze_tapped) {
    ring_snooze_tapped = false;
    stop_timer_ring("timer_snoozed", "a tap", true);
  }
  if (note_seen) {
    note_seen = false;
    clear_notifications(true);
  }
  if (note_reopen) {
    note_reopen = false;
    if (!notes.empty()) {
      board::markActivity();
      ui::showNotification(notes.back().title.c_str(), notes.back().text.c_str(), 12, [] { note_seen = true; });
    }
  }
  static uint32_t last_row = 0;
  if (millis() - last_row >= 1000) {
    last_row = millis();
    refresh_status_row();
  }
  timers::Item due;
  if (!timer_ring.on && voice == Voice::Off && !ring_id.length() && timers::takeDue(&due)) start_timer_ring(due);
  if (timer_ring.on) {
    const uint32_t limit = timer_ring.item.alarm ? ALARM_RING_MS : TIMER_RING_MS;
    if (wakeword::takeStop()) {
      stop_timer_ring("timer_stopped", "\"stop\"", false);
    } else if (millis() - timer_ring.started > limit) {
      stop_timer_ring("timer_missed", "nobody stopped it", false);
    } else if (static_cast<int32_t>(millis() - timer_ring.next_sound) >= 0 && !audio::playing()) {
      audio::play(timer_ring.item.alarm ? audio::Sound::Alarm : audio::Sound::TimerDone);
      timer_ring.next_sound = millis() + RING_SOUND_EVERY_MS;
      board::markActivity();  // the screen stays awake while it rings
    }
  }
  wakeword::setStopListening(timer_ring.on && !audio::micMuted());
  if (timers::version() != timers_sent) send_timers();
}

void update_voice() {
  if (tap_requested) {
    tap_requested = false;
    handle_tap();
  }
  // The wake word listens only while the speaker is idle: not in a conversation, ringing, muted or making a sound
  // (it would hear itself).
  const bool idle = voice == Voice::Off && !ring_id.length() && !audio::micMuted() && !audio::speaking() &&
                    !audio::playing();
  wakeword::setListening(idle);
  const bool woke = wakeword::takeDetection() && idle;
  if (woke && timer_ringing()) {
    stop_timer_ring("timer_stopped", "the wake word", false);  // instead of starting a conversation
  } else if (woke && ws_connected) {
    if (wake_arbitration && audio::ultrasonic()) {
      arbitrate();
    } else {
      board::markActivity();  // wake the screen
      start_listening_turn();
    }
  }
  update_arbitration();
  static uint32_t last_stats = 0;
  if (wakeword::loaded() && millis() - last_stats > 60000) {
    last_stats = millis();
    const wakeword::Stats st = wakeword::stats();
    const audio::MicStats mic = audio::micStats();
    LOGI(TAG, "mic (%s kHz): average %d dBFS, loudest %d dBFS, noise floor %d", audio::ultrasonic() ? "48" : "16",
         mic.average_db, mic.loudest_db, mic.noise_floor);
    LOGI(TAG, "wake word: %u inferences in the last minute, %u us each, highest %u%%", st.inferences,
         st.avg_infer_us, static_cast<unsigned>(st.max_probability * 100));
  }
  if (voice == Voice::Starting && audio::micMuted()) {  // muted before listening began
    voice_done();
  } else if (voice == Voice::Starting && static_cast<int32_t>(millis() - voice_at) >= 0) {
    if (!ws_connected) voice_failed("not connected");
    else if (audio::startListening(MAX_LISTEN_MS, silence_ms)) start_voice();
    else voice_failed("couldn't start listening");
  } else if (voice == Voice::Listening) {
    if (ws_connected) pump_mic();
    else voice_failed("connection lost while listening");
  } else if (voice == Voice::Waiting && static_cast<int32_t>(millis() - voice_at) >= 0) {
    send_voice("voice_cancel");
    voice_failed("no answer from Harkenn");
  } else if (voice == Voice::Speaking && !audio::speaking()) {
    if (listen_after_speech) {  // a question was asked: listen for the answer
      listen_after_speech = false;
      if (audio::micMuted()) {
        send_voice("voice_cancel");
        voice_done();
        show_status_text("Mic is off", 2500);
      } else {
        start_listening_turn(true, follow_up_next);
      }
    } else {
      voice_done();
    }
  }
  if (ring_id.length()) {
    if (static_cast<int32_t>(millis() - ring_until) >= 0) {
      stop_ringing();
      voice_done();
    } else if (static_cast<int32_t>(millis() - ring_next) >= 0) {
      audio::play(audio::Sound::Ring);
      ring_next += RING_EVERY_MS;
    }
  }  if (status_text_until && static_cast<int32_t>(millis() - status_text_until) >= 0) {
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
    send_text("{\"type\":\"pong\"}");
  } else if (type == "timer") {
    handle_timer(doc);
  } else if (type == "media") {
    handle_media(doc);
  } else if (type == "notify") {
    handle_notify(doc);
  } else if (type == "notifications_clear") {
    clear_notifications(false);
  } else if (type == "fetch_result") {
    fetch_result(doc);
  } else if (type == "local_changes_ack") {
    store::clearLocalChanges(doc["id"] | 0);  // the config Harkenn sends next includes them
  } else if (type.startsWith("speak_") || type.startsWith("voice_") || type.startsWith("ring") ||
             type == "listen_stop") {
    handle_voice_message(type, doc);
  } else if (type == "screen") {
    auto sc = std::unique_ptr<PendingScreen>(new PendingScreen());
    sc->id = doc["screen"] | "";
    sc->url = doc["url"] | "";
    sc->tap_dismisses = String(doc["dismiss"] | "tap") != "none";
    sc->seconds = constrain(doc["seconds"] | 60, 5, 600);
    for (JsonObject r : doc["regions"].as<JsonArray>()) {
      ui::TapRegion t;
      t.label = r["label"] | "";
      t.x = r["x"] | 0;
      t.y = r["y"] | 0;
      t.w = r["w"] | 0;
      t.h = r["h"] | 0;
      if (t.label.length() && t.w > 0 && t.h > 0) sc->regions.push_back(t);
    }
    const size_t bytes = doc["bytes"] | 0;
    receiving_screen.reset();
    if (bytes > 0 && bytes <= MAX_ASSET_BYTES) {  // the picture follows in binary frames
      sc->image = std::make_shared<ui::Blob>();
      sc->image->data = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
      if (sc->image->data) {
        sc->image->size = bytes;
        receiving_got = 0;
        receiving_screen = std::move(sc);
      } else {
        LOGW(TAG, "screen: out of PSRAM for %u bytes", static_cast<unsigned>(bytes));
      }
    } else if (sc->url.length()) {
      pending_screen = std::move(sc);  // older servers: downloaded from the main loop
    }
  } else if (type == "screen_clear") {
    pending_screen.reset();
    ui::hideScreen();
  } else if (type == "update_available") {
    JsonObject f = doc["firmware"];
    auto fw = std::unique_ptr<FirmwareUpdate>(new FirmwareUpdate());
    fw->url = f["url"] | "";
    fw->version = f["version"] | "";
    fw->sha256 = f["sha256"] | "";
    fw->size = f["size"] | 0;
    if (!fw->version.length() || fw->version == FW_VERSION) return;
    if (fw->version == store::dismissedUpdate()) {
      LOGI(TAG, "firmware %s available (dismissed earlier)", fw->version.c_str());
      return;
    }
    LOGI(TAG, "firmware %s available", fw->version.c_str());
    strlcpy(offered_version, fw->version.c_str(), sizeof(offered_version));
    ui::showUpdateOffer(fw->version.c_str(), [] { offer_install = true; }, [] { offer_dismiss = true; });
    board::markActivity();
    offered_update = std::move(fw);
  } else if (type == "command") {
    const String command = doc["command"] | "";
    JsonObject args = doc["args"];
    LOGI(TAG, "command %s", command.c_str());
    if (command == "reboot") {
      restart();
    } else if (command == "factory_reset") {
      factory_reset_wanted = true;  // from the main loop
    } else if (command == "card_rescan") {
      card_rescan_wanted = true;  // from the main loop
    } else if (command == "card_format") {
      card_format_wanted = true;  // from the main loop
    } else if (command == "identify") {
      ui::identify();
    } else if (command == "refresh_config") {
      want_config = true;
    } else if (command == "update_firmware") {
      auto fw = std::unique_ptr<FirmwareUpdate>(new FirmwareUpdate());
      fw->url = args["url"] | "";
      fw->version = args["version"] | "?";
      fw->sha256 = args["sha256"] | "";
      fw->size = args["size"] | 0;
      pending_update = std::move(fw);  // run from the main loop, not inside the socket callback
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

// Talking with Harkenn: directly, or through the relay once the handshake is done.
void on_connected() {
  ws_connected = true;
  ws_down_since = 0;
  first_ws_config = true;
  local_changes_due = true;
  timers_sent = UINT32_MAX;  // tell Harkenn what's set
  card_sent = UINT32_MAX;
  media_sent = UINT32_MAX;
  LOGI(TAG, via_relay ? "connected to Harkenn through the relay" : "connected to Harkenn");
  // This firmware reached the server: keep it (cancels rollback when the bootloader supports it).
  esp_ota_mark_app_valid_cancel_rollback();
  send_settings();  // in case the volume changed while offline
  send_status();
  if (!update_checked) {  // once per boot
    update_checked = true;
    send_text("{\"type\":\"update_check\"}");
  }
}

void handle_binary(const uint8_t* payload, size_t len) {
  if (receiving_screen) {  // a screen's picture
    ui::Blob& b = *receiving_screen->image;
    const size_t n = min(len, b.size - receiving_got);
    memcpy(b.data + receiving_got, payload, n);
    receiving_got += n;
    if (receiving_got == b.size) pending_screen = std::move(receiving_screen);
  } else if (voice == Voice::Speaking) {  // speech for the current reply
    speech_dropped += len - audio::writeSpeech(payload, len);
  }
}

// A binary frame from the relay: Harkenn's side of the handshake, then sealed frames.
void relay_frame(uint8_t* payload, size_t len) {
  if (relay_state == RelayState::Waiting) {
    uint8_t answer[33];
    if (relay::answer(payload, len, answer)) {
      ws.sendBIN(answer, sizeof(answer));
      relay_state = RelayState::Confirming;
    }
    return;
  }
  if (relay_state == RelayState::Confirming) {
    if (!relay::confirm(payload, len)) {
      LOGW(TAG, "relay: the other side doesn't have the key");
      ws.disconnect();
      return;
    }
    relay_state = RelayState::Ready;
    JsonDocument hello;  // proves it's us, like the Authorization header of a direct connection
    hello["type"] = "hello";
    hello["token"] = token;
    send_json(hello);
    on_connected();
    return;
  }
  char kind;
  uint8_t* data;
  size_t n;
  if (relay_state != RelayState::Ready || !relay::open(payload, len, &kind, &data, &n)) {
    LOGW(TAG, "relay: a frame that doesn't check out; starting over");
    ws.disconnect();
    return;
  }
  if (kind == 'T') handle_text(data, n);
  else if (kind == 'B') handle_binary(data, n);
  else if (kind == 'F') fetch_data(data, n);
}

void on_ws_event(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      if (via_relay) {
        relay_state = RelayState::Waiting;  // for Harkenn to come, then its HELLO
        LOGI(TAG, "at the relay, waiting for Harkenn");
      } else {
        on_connected();
      }
      break;
    case WStype_DISCONNECTED:
      if (ws_connected) LOGW(TAG, "connection to Harkenn lost");
      ws_connected = false;
      if (via_relay) {
        relay_state = RelayState::Off;
        relay::end();
        relay::begin(store::remote().key);  // ready for the next try
      }
      if (voice == Voice::Speaking) audio::endSpeech();  // play what arrived
      receiving_screen.reset();                           // a picture cut off halfway
      if (!ws_down_since) ws_down_since = millis();
      break;
    case WStype_TEXT:
      if (!via_relay) handle_text(payload, len);  // the relay's own text ("paired") needs nothing from us
      break;
    case WStype_BIN:
      if (via_relay) relay_frame(payload, len);
      else handle_binary(payload, len);
      break;
    default:
      break;
  }
}

void run_firmware_update() {
  std::unique_ptr<FirmwareUpdate> fw = std::move(pending_update);
  LOGI(TAG, "updating firmware %s -> %s (%u bytes)", FW_VERSION, fw->version.c_str(), fw->size);
  flush_logs();
  voice = Voice::Off;
  audio::stopListening();
  audio::stopSpeech();
  if (!via_relay) {  // downloaded over HTTP; through the relay it comes on this connection
    ws.disconnect();
    ws_connected = false;
  }
  ui::showProgress("Updating firmware", fw->version.c_str(), 0);
  const String error = install_firmware(*fw);
  if (error.length()) {
    LOGE(TAG, "firmware update failed: %s", error.c_str());
    audio::play(audio::Sound::Error);
    ui::showMessage("Update failed", error.c_str(), false);
    delay(4000);
    return;  // run_online() starts over: reconnects and redraws the layout
  }
  store::setUpdatedFrom(FW_VERSION);
  ui::showMessage("Update installed", "Restarting", true);
  restart();
}

// After sleep_after_ms without a touch the screen sleeps (config display.sleep_display): dimmed with the character
// sleeping, dimmed with only a big clock, or off. A touch, the wake word or anything Harkenn shows wakes it.
void update_sleep() {
  if (!sleep_after_ms) return;
  const bool idle = millis() - board::lastTouchMs() > sleep_after_ms;
  if (idle && !dimmed) {
    dimmed = true;
    board::setBrightness(sleep_display == "off" ? 0 : max(5, brightness / 8));
    if (sleep_display == "clock") ui::showSleepClock(true);
    if (strcmp(ui::state(), "idle") == 0) ui::setState("sleeping");
    send_status();
  } else if (!idle && dimmed) {
    dimmed = false;
    ui::showSleepClock(false);
    board::setBrightness(brightness);
    if (strcmp(ui::state(), "sleeping") == 0) ui::setState("idle");
    send_status();
  }
}

// Returns false if the server rejected the token (re-adopt), true for anything else (start over).
// Harkenn answers directly at the saved addresses (a quick check while on the relay).
bool direct_reachable() {
  for (const String& url : {store::serverUrl(), store::announcedServerUrl()}) {
    if (!url.length()) continue;
    server_url = url;
    server = parse_url(url);
    if (ping_server()) return true;
  }
  return false;
}

// Leaving relay mode, however run_online ends.
struct RelayMode {
  ~RelayMode() {
    via_relay = false;
    relay_state = RelayState::Off;
    relay::end();
    ui::setRemote(false);
  }
};

// Returns false if the server rejected the token (re-adopt), true for anything else (start over). `use_relay`: the
// same conversation through the relay, for a speaker set up for remote connection that can't reach Harkenn; it ends
// when Harkenn is reachable directly again.
bool run_online(bool use_relay = false) {
  RelayMode relay_mode;
  if (use_relay) {
    const store::Remote remote = store::remote();
    if (!remote.url.length() || !relay::begin(remote.key)) return true;
    String url = remote.url;  // wss://host[:port][/base]
    if (url.startsWith("wss://")) url = "https://" + url.substring(6);
    else if (url.startsWith("ws://")) url = "http://" + url.substring(5);
    const ServerUrl relay_url = parse_url(url);
    const String path = relay_url.base + "/relay/speaker/" + remote.id;
    via_relay = true;
    ui::setRemote(true);
    LOGI(TAG, "trying the relay at %s", remote.url.c_str());
    if (relay_url.secure) ws.beginSslWithCA(relay_url.host.c_str(), relay_url.port, path.c_str(), CA_PEM, "");
    else ws.begin(relay_url.host, relay_url.port, path, "");
    ws.setExtraHeaders("");  // the token goes inside, where the relay can't see it
  } else {
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
  }
  ws.onEvent(on_ws_event);
  uint32_t direct_checked = millis();
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
      send_text("{\"type\":\"config_request\"}");
    }
    // asked for on the web page
    if (local_changes_due && ws_connected) send_local_changes();
    if (update_check_wanted && ws_connected) {
      update_check_wanted = false;
      send_text("{\"type\":\"update_check\"}");
    }
    if (wifi_changed) {
      wifi_changed = false;
      rebuild_wifi();
      apply_static_ip();
    }
    if (ws_connected && web::hotspotOn()) stop_hotspot();  // back with Harkenn: the page is on the network now
    if (reconnect_wanted) {
      reconnect_wanted = false;
      ws.disconnect();
      return true;  // starts over with the new server address
    }
    if (audio_restart_wanted && voice == Voice::Off) {
      LOGI(TAG, "only the nearest answers: %s; restarting so the audio runs at %s kHz", wake_arbitration ? "on" : "off",
           wake_arbitration ? "48" : "16");
      flush_logs();
      restart();
    }
    if (factory_reset_wanted) {
      LOGW(TAG, "factory reset requested by Harkenn");
      flush_logs();
      ws.disconnect();
      ui::showMessage("Factory reset", "Starting over", false);
      store::factoryReset();
      delay(1000);
      ESP.restart();
    }
    if (card_format_wanted) {
      card_format_wanted = false;
      player::stop();
      delay(150);  // let it close its file
      card::format();
      send_status();
    }
    if (card_rescan_wanted) {
      card_rescan_wanted = false;
      player::stop();
      delay(150);  // let it close its file
      card::rescan();
      send_status();
    }
    if (ws_connected && card_sent != card::version()) send_card();
    if (ws_connected && media_sent != player::version()) send_media();
    // Music fades out while the speaker listens, talks or rings, and comes back after.
    audio::holdMusic(audio::HOLD_VOICE, voice != Voice::Off || ring_id.length() || timer_ringing());
    if (ws_connected && millis() - last_status > STATUS_INTERVAL_MS) send_status();
    if (millis() - last_log_flush > LOG_FLUSH_MS) flush_logs();
    update_voice();
    update_timers();
    update_screen();
    update_sleep();
    if (offer_install && offered_update) {
      LOGI(TAG, "installing firmware %s", offered_update->version.c_str());
      ui::hideUpdateOffer();
      pending_update = std::move(offered_update);
      offered_version[0] = 0;
    } else if (offer_dismiss && offered_update) {
      LOGI(TAG, "firmware %s dismissed", offered_update->version.c_str());
      store::setDismissedUpdate(offered_update->version);
      offered_update.reset();
      offered_version[0] = 0;
    }
    offer_install = offer_dismiss = false;
    if (pending_update) {
      run_firmware_update();  // only returns if the update failed
      return true;
    }
    if (play_state_until && static_cast<int32_t>(millis() - play_state_until) > 0) {
      play_state_until = 0;
      ui::setState(play_state_restore.c_str());
      send_status();
    }
    if (WiFi.status() != WL_CONNECTED) {
      ws.disconnect();
      return true;
    }
    if (use_relay) {
      if (!store::remote().url.length()) {  // remote connection was turned off
        ws.disconnect();
        return true;
      }
      // Every few minutes, while nothing's going on, see whether Harkenn can be reached directly again.
      if (millis() - direct_checked > DIRECT_RETRY_MS && voice == Voice::Off && !ring_id.length()) {
        direct_checked = millis();
        if (direct_reachable()) {
          LOGI(TAG, "Harkenn is reachable directly again: leaving the relay");
          ws.disconnect();
          return true;
        }
      }
      if (!ws_connected && ws_down_since && millis() - ws_down_since > RELAY_GIVE_UP_MS) {
        ws.disconnect();
        return true;  // tries Harkenn directly, then the relay again
      }
    } else if (!ws_connected && ws_down_since && millis() - ws_down_since > WS_GIVE_UP_MS) {
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

// BOOT button: a short press mutes or unmutes the mic. Hold 3 s to power off (a press turns it back on), 6 s for
// Wi-Fi setup, 11 s for a factory reset.
void button_task(void*) {
  uint32_t down_at = 0;
  int stage = 0;
  for (;;) {
    if (board::buttonDown()) {
      if (!down_at) down_at = millis();
      const uint32_t held = millis() - down_at;
      if (held > 11000 && stage < 3) {
        stage = 3;
        ui::showMessage("Release to factory reset", "Forgets Wi-Fi and adoption", false);
      } else if (held > 6000 && stage < 2) {
        stage = 2;
        ui::showMessage("Release for Wi-Fi setup", "Keep holding to factory reset", false);
      } else if (held > 3000 && stage < 1) {
        stage = 1;
        ui::showMessage("Release to power off", "Keep holding for Wi-Fi setup", false);
      }
    } else if (down_at) {
      if (stage == 1) {
        power_off();
      } else if (stage == 2) {
        store::requestPortal();
        ESP.restart();
      } else if (stage == 3) {
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

// The web page's view of the speaker, and what it can ask for (it runs in its own task).
void start_web() {
  web::Hooks h;
  h.status = [](JsonObject s) {
    s["name"] = device_name;
    s["device_id"] = device_id;
    s["firmware"] = FW_VERSION;
    s["hardware"] = HARDWARE_ID;
    s["adopted"] = token.length() > 0;
    s["connected"] = ws_connected;
    s["via_relay"] = via_relay;
    s["server"] = server_url;
    s["ap_name"] = ap_name();
    s["ssid"] = WiFi.isConnected() ? WiFi.SSID() : String();
    if (WiFi.isConnected()) s["rssi"] = WiFi.RSSI();
    s["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String();
    s["state"] = ui::state();
    s["volume"] = static_cast<int>(volume);
    s["muted"] = audio::micMuted();
    s["brightness"] = brightness;
    s["battery"] = board::batteryPercent();
    s["charging"] = board::charging();
    s["uptime_s"] = millis() / 1000;
    add_memory(s);
    if (offered_version[0]) s["update_offered"] = offered_version;
  };
  h.set_volume = [](int v) { set_volume_local(v); };
  h.set_brightness = [](int b) {
    brightness = b;
    if (!dimmed) board::setBrightness(b);
  };
  h.wifi_changed = [] { wifi_changed = true; };
  h.server_changed = [] { reconnect_wanted = true; };
  h.changes_recorded = [] { local_changes_due = true; };
  h.check_update = [] {
    if (!ws_connected) return false;
    store::setDismissedUpdate("");  // asked for: offer it even if it was dismissed on the screen
    update_check_wanted = true;
    return true;
  };
  h.install_offered = [] {
    if (!offered_version[0]) return false;
    offer_install = true;
    return true;
  };
  h.restart = [] { restart(); };
  web::begin(h);
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
  LOGI(TAG, "Harkenn speaker %s, firmware %s, server %s, %s", device_id.c_str(), FW_VERSION, server_url.c_str(),
       token.length() ? "adopted" : "not adopted");
  const String updated_from = store::takeUpdatedFrom();
  if (updated_from.length()) LOGI(TAG, "firmware updated from %s to %s", updated_from.c_str(), FW_VERSION);

  volume = store::volume(70);
  audio::soundsBegin();  // the sound pack's clips, before the boot sound plays
  wakeword::begin();
  timers::begin();
  ui::onNotificationTap([] { note_reopen = true; });
  if (audio::begin(store::ultrasonic())) {
    audio::setVolume(volume);
    audio::setMicMuted(store::micMuted());
  }
  ui::setMuted(audio::micMuted());
  ui::onScreenTap([](const char* label) {
    strlcpy(screen_tap_label, label, sizeof(screen_tap_label));
    screen_tapped = true;
  });
  ui::onTouch([] { tap_requested = true; },
              [](int delta) {
                const int before = volume;
                set_volume_local(before + delta);
                if (volume / 5 != before / 5) audio::play(audio::Sound::Tick);
                return static_cast<int>(volume);
              });

  ui::showMessage("Starting", ap_name().c_str());
  card::begin();
  player::begin();
  board::setBrightness(brightness);
  start_web();
  bool physical = store::takePortalRequest();  // held the button for Wi-Fi setup
  rebuild_wifi();

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      connect_wifi(physical);
      physical = false;
    }
    // Set up for remote connection (and adopted): after a while without Harkenn, go through the relay.
    const bool remote = token.length() && store::remote().url.length();
    if (!find_server(remote ? RELAY_AFTER_MS : 0)) {
      if (remote && WiFi.status() == WL_CONNECTED) {
        LOGI(TAG, "can't find Harkenn: trying the relay");
        run_online(true);
        delay(3000);
      }
      continue;
    }
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
  xTaskCreatePinnedToCore(main_task, "harkenn", 20480, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(button_task, "button", 4096, nullptr, 1, nullptr, 1);
}

}  // namespace app
