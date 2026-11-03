#include "web.h"

#include <DNSServer.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/pkcs5.h>

#include <functional>

#include "logbuf.h"
#include "store.h"

namespace web {
namespace {

constexpr const char* TAG = "web";
constexpr uint32_t SESSION_IDLE_MS = 15 * 60 * 1000;
constexpr uint8_t MAX_FAILURES = 10;  // then locked until Harkenn unlocks it
constexpr const char* MARKER = "HARKENN-FW|";

extern const char PAGE[];

Hooks hooks;
WebServer* server = nullptr;
DNSServer* dns = nullptr;
volatile bool hotspot = false, hotspot_physical = false;
String session;  // the logged-in browser's cookie; empty = nobody
uint32_t session_used = 0;
volatile uint32_t last_use = 0;
volatile uint8_t fail_count = 0;
volatile bool fail_locked = false;
uint32_t wait_until = 0;  // millis() before which no password is checked; 0 = none

// 3 wrong passwords: 30 s, doubling every other try (5: 60 s, 7: 120 s, 9: 240 s).
uint32_t wait_s(uint8_t failures) { return failures < 3 ? 0 : 30u << ((failures - 3) / 2); }

uint32_t waiting_s() {
  if (!wait_until) return 0;
  const int32_t left = static_cast<int32_t>(wait_until - millis());
  return left > 0 ? (left + 999) / 1000 : 0;
}

// "pbkdf2_sha256$rounds$salt$digest" (base64), as made by Harkenn's local_access.py.
bool check_hash(const String& password, const String& stored) {
  const int a = stored.indexOf('$'), b = stored.indexOf('$', a + 1), c = stored.indexOf('$', b + 1);
  if (a < 0 || b < 0 || c < 0 || stored.substring(0, a) != "pbkdf2_sha256") return false;
  const long rounds = stored.substring(a + 1, b).toInt();
  if (rounds < 1000 || rounds > 1000000) return false;
  const String salt64 = stored.substring(b + 1, c), digest64 = stored.substring(c + 1);
  uint8_t salt[48], want[48], got[32];
  size_t salt_len = 0, want_len = 0;
  if (mbedtls_base64_decode(salt, sizeof(salt), &salt_len, reinterpret_cast<const uint8_t*>(salt64.c_str()),
                            salt64.length()) ||
      mbedtls_base64_decode(want, sizeof(want), &want_len, reinterpret_cast<const uint8_t*>(digest64.c_str()),
                            digest64.length()) ||
      want_len != sizeof(got)) {
    return false;
  }
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(password.c_str()),
                                    password.length(), salt, salt_len, rounds, sizeof(got), got)) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(got); i++) diff |= got[i] ^ want[i];
  return diff == 0;
}

bool has_passwords() {
  const store::LocalAccess a = store::localAccess();
  return a.admin_hash.length() || a.speaker_hash.length();
}

// Open without a password: a speaker that isn't adopted (first-time setup), or one that never got its passwords from
// Harkenn when someone holding it opened Wi-Fi setup with the button.
bool open_access() { return !store::token().length() || (hotspot_physical && !has_passwords()); }

String cookie_session() {
  const String cookies = server->header("Cookie");
  const int at = cookies.indexOf("hk=");
  if (at < 0) return "";
  const int end = cookies.indexOf(';', at);
  return cookies.substring(at + 3, end < 0 ? cookies.length() : end);
}

bool logged_in() {
  if (!session.length() || millis() - session_used > SESSION_IDLE_MS) return false;
  return cookie_session() == session;
}

void send_json(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server->sendHeader("Cache-Control", "no-store");
  server->send(code, "application/json", out);
}

void send_error(int code, const String& message) {
  JsonDocument doc;
  doc["error"] = message;
  send_json(code, doc);
}

void send_ok() {
  JsonDocument doc;
  doc["ok"] = true;
  send_json(200, doc);
}

// Every API call: logged in (or open), and POSTs only from the page's own script (a header no other site can add).
bool guard() {
  if (server->method() == HTTP_POST && server->header("X-Requested-With") != "harkenn") {
    send_error(403, "missing X-Requested-With header");
    return false;
  }
  if (!open_access() && !logged_in()) {
    send_error(401, "log in first");
    return false;
  }
  session_used = millis();
  last_use = session_used;
  return true;
}

bool body(JsonDocument& doc) {
  if (deserializeJson(doc, server->arg("plain")) || !doc.is<JsonObject>()) {
    send_error(400, "expected a JSON object");
    return false;
  }
  return true;
}

constexpr size_t MAX_REMOVED = 16;  // network names removed here, remembered until Harkenn has them

// Local changes: kept until Harkenn acknowledges them (app.cpp sends them). The lists are capped, so a speaker that
// never reaches Harkenn can't outgrow its NVS: added networks to the saved list's size (newest first), removed names
// to the newest MAX_REMOVED.
void record(const std::function<void(JsonObject)>& edit) {
  JsonDocument doc;
  if (deserializeJson(doc, store::localChanges()) || !doc.is<JsonObject>()) doc.to<JsonObject>();
  edit(doc.as<JsonObject>());
  JsonArray added = doc["wifi"].as<JsonArray>(), removed = doc["wifi_removed"].as<JsonArray>();
  while (added.size() > store::MAX_WIFI) added.remove(added.size() - 1);
  while (removed.size() > MAX_REMOVED) removed.remove(0);
  String out;
  serializeJson(doc, out);
  store::setLocalChanges(out);
  if (hooks.changes_recorded) hooks.changes_recorded();
}

void drop_ssid(JsonArray list, const String& ssid) {
  for (size_t i = list.size(); i-- > 0;) {
    JsonVariant v = list[i];
    if ((v.is<JsonObject>() ? String(v["ssid"] | "") : String(v | "")) == ssid) list.remove(i);
  }
}

bool valid_ip(const String& s) {
  IPAddress ip;
  return ip.fromString(s);
}

// handlers

void handle_session() {
  JsonDocument doc;
  doc["open"] = open_access();
  doc["logged_in"] = logged_in();
  doc["passwords"] = has_passwords();
  doc["locked"] = fail_locked;
  doc["wait_s"] = waiting_s();
  doc["failures"] = fail_count;
  doc["hotspot"] = static_cast<bool>(hotspot);
  send_json(200, doc);
}

void handle_login() {
  if (server->header("X-Requested-With") != "harkenn") return send_error(403, "missing X-Requested-With header");
  JsonDocument in;
  if (!body(in)) return;
  JsonDocument out;
  if (fail_locked) {
    out["error"] = "Locked after too many wrong passwords. Unlock it in Harkenn, or factory reset the speaker.";
    out["locked"] = true;
    return send_json(423, out);
  }
  if (const uint32_t w = waiting_s()) {
    out["error"] = "Too many wrong passwords: wait " + String(w) + " s";
    out["wait_s"] = w;
    return send_json(429, out);
  }
  const store::LocalAccess a = store::localAccess();
  if (!a.admin_hash.length() && !a.speaker_hash.length()) {
    return send_error(503, "This speaker hasn't had its passwords from Harkenn yet. Set a speaker password in Harkenn, "
                           "or factory reset it with the button.");
  }
  const String password = in["password"] | "";
  const uint32_t started = millis();
  const char* which = nullptr;
  if (password.length()) {
    if (a.admin_hash.length() && check_hash(password, a.admin_hash)) which = "admin";
    else if (a.speaker_hash.length() && check_hash(password, a.speaker_hash)) which = "speaker";
  }
  LOGD(TAG, "password checked in %u ms", static_cast<unsigned>(millis() - started));
  if (!which) {
    fail_count = fail_count + 1;
    store::setLoginFailures(fail_count);
    if (fail_count >= MAX_FAILURES) {
      fail_locked = true;
      store::setLoginLocked(true);
      LOGW(TAG, "wrong password %u times: locked until Harkenn unlocks it", fail_count);
      out["error"] = "Wrong password. The page is now locked: unlock it in Harkenn, or factory reset the speaker.";
      out["locked"] = true;
      return send_json(423, out);
    }
    const uint32_t w = wait_s(fail_count);
    wait_until = w ? millis() + w * 1000 : 0;
    LOGW(TAG, "wrong password (%u in a row)", fail_count);
    out["error"] = w ? "Wrong password: wait " + String(w) + " s before trying again" : String("Wrong password");
    out["wait_s"] = w;
    out["tries_left"] = MAX_FAILURES - fail_count;
    return send_json(401, out);
  }
  fail_count = 0;
  store::setLoginFailures(0);
  wait_until = 0;
  char token[33];
  for (int i = 0; i < 16; i++) snprintf(token + 2 * i, 3, "%02x", static_cast<unsigned>(esp_random() & 0xff));
  session = token;
  session_used = millis();
  last_use = session_used;
  LOGI(TAG, "logged in with the %s password", which);
  server->sendHeader("Set-Cookie", "hk=" + session + "; Path=/; HttpOnly; SameSite=Strict");
  send_ok();
}

void handle_logout() {
  if (!guard()) return;
  session = "";
  server->sendHeader("Set-Cookie", "hk=; Path=/; Max-Age=0");
  send_ok();
}

void handle_status() {
  if (!guard()) return;
  JsonDocument doc;
  JsonObject s = doc.to<JsonObject>();
  if (hooks.status) hooks.status(s);
  s["server_setting"] = store::serverUrl();
  const char level = logbuf::uploadLevel();
  s["log_level"] = level == 'E' ? "error" : level == 'W' ? "warn" : level == 'D' ? "debug" : "info";
  s["local_changes"] = store::localChanges().length() > 0;
  s["hotspot"] = static_cast<bool>(hotspot);
  s["open"] = open_access();
  send_json(200, doc);
}

void handle_settings() {
  if (!guard()) return;
  JsonDocument in;
  if (!body(in)) return;
  if (in["volume"].is<int>() && hooks.set_volume) hooks.set_volume(constrain(in["volume"].as<int>(), 0, 100));
  if (in["brightness"].is<int>()) {
    const int b = constrain(in["brightness"].as<int>(), 5, 100);
    if (hooks.set_brightness) hooks.set_brightness(b);
    record([b](JsonObject c) { c["brightness"] = b; });
  }
  if (in["log_level"].is<const char*>()) {
    const String level = in["log_level"].as<const char*>();
    if (level != "error" && level != "warn" && level != "info" && level != "debug") {
      return send_error(422, "log level: error, warn, info or debug");
    }
    logbuf::setUploadLevel(level == "error" ? 'E' : level == "warn" ? 'W' : level == "debug" ? 'D' : 'I');
    record([level](JsonObject c) { c["log_level"] = level; });
    LOGI(TAG, "log level %s", level.c_str());
  }
  if (in["server_url"].is<const char*>()) {
    String url = in["server_url"].as<const char*>();
    url.trim();
    if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) {
      return send_error(422, "the server address starts with http:// or https:// (or leave it empty to find it)");
    }
    if (url != store::serverUrl()) {
      store::setServerUrl(url);
      record([url](JsonObject c) { c["server_url"] = url; });
      LOGI(TAG, "server address set to %s", url.length() ? url.c_str() : "(find it)");
      if (hooks.server_changed) hooks.server_changed();
    }
  }
  send_ok();
}

void handle_wifi_list() {
  if (!guard()) return;
  JsonDocument doc;
  JsonArray list = doc["networks"].to<JsonArray>();
  for (const auto& n : store::wifiNetworks()) {
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = n.ssid;
    o["open"] = !n.password.length();
    o["ip"] = n.ip;
    o["subnet"] = n.subnet;
    o["gateway"] = n.gateway;
    o["dns"] = n.dns;
  }
  doc["current"] = WiFi.isConnected() ? WiFi.SSID() : String();
  doc["max"] = store::MAX_WIFI;
  send_json(200, doc);
}

void handle_wifi_save() {
  if (!guard()) return;
  JsonDocument in;
  if (!body(in)) return;
  store::WifiNetwork n;
  n.ssid = in["ssid"] | "";
  n.ssid.trim();
  if (!n.ssid.length() || n.ssid.length() > 32) return send_error(422, "the network name is 1-32 characters");
  bool known = false;
  for (const auto& w : store::wifiNetworks()) {
    if (w.ssid == n.ssid) {
      n.password = w.password;
      known = true;
    }
  }
  if (!in["password"].isNull() || !known) n.password = in["password"] | "";  // null: keep the saved one
  if (n.password.length() && (n.password.length() < 8 || n.password.length() > 64)) {
    return send_error(422, "Wi-Fi passwords are 8-63 characters (empty for an open network)");
  }
  n.ip = in["ip"] | "";
  n.subnet = in["subnet"] | "";
  n.gateway = in["gateway"] | "";
  n.dns = in["dns"] | "";
  for (String* s : {&n.ip, &n.subnet, &n.gateway, &n.dns}) s->trim();
  if (n.ip.length()) {
    if (!valid_ip(n.ip) || !valid_ip(n.subnet) || !valid_ip(n.gateway) || (n.dns.length() && !valid_ip(n.dns))) {
      return send_error(422, "a static IP needs a valid address, subnet mask and gateway (DNS is optional)");
    }
  } else {
    n.subnet = n.gateway = n.dns = "";
  }
  if (store::upsertWifi(n)) {
    record([&n](JsonObject c) {
      drop_ssid(c["wifi_removed"].as<JsonArray>(), n.ssid);
      JsonArray old = c["wifi"].as<JsonArray>();
      JsonDocument keep;
      JsonArray list = keep.to<JsonArray>();
      JsonObject o = list.add<JsonObject>();
      o["ssid"] = n.ssid;
      o["password"] = n.password;
      o["ip"] = n.ip;
      o["subnet"] = n.subnet;
      o["gateway"] = n.gateway;
      o["dns"] = n.dns;
      for (JsonObject w : old) {
        if (String(w["ssid"] | "") != n.ssid) list.add(w);
      }
      c["wifi"] = list;
    });
    LOGI(TAG, "Wi-Fi %s saved%s", n.ssid.c_str(), n.ip.length() ? (", static " + n.ip).c_str() : "");
    if (hooks.wifi_changed) hooks.wifi_changed();
  }
  send_ok();
}

void handle_wifi_remove() {
  if (!guard()) return;
  JsonDocument in;
  if (!body(in)) return;
  const String ssid = in["ssid"] | "";
  if (!store::removeWifi(ssid)) return send_error(404, "no such network");
  record([&ssid](JsonObject c) {
    drop_ssid(c["wifi"].as<JsonArray>(), ssid);
    JsonArray removed = c["wifi_removed"].is<JsonArray>() ? c["wifi_removed"].as<JsonArray>()
                                                          : c["wifi_removed"].to<JsonArray>();
    drop_ssid(removed, ssid);
    removed.add(ssid);
  });
  LOGI(TAG, "Wi-Fi %s removed", ssid.c_str());
  if (hooks.wifi_changed) hooks.wifi_changed();
  send_ok();
}

void handle_scan() {
  if (!guard()) return;
  const int found = WiFi.scanNetworks();
  if (found < 0) return send_error(503, "the speaker is busy joining a network: try again in a moment");
  JsonDocument doc;
  JsonArray list = doc["networks"].to<JsonArray>();
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool seen = false;
    for (JsonObject o : list) seen |= String(o["ssid"] | "") == ssid;
    if (seen) continue;  // strongest first: keep the first of each name
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  send_json(200, doc);
}

void handle_logs() {
  if (!guard()) return;
  JsonDocument doc;
  doc["newest"] = logbuf::recent(doc["lines"].to<JsonArray>(), server->arg("after").toInt(), 120);
  send_json(200, doc);
}

void handle_restart() {
  if (!guard()) return;
  send_ok();
  LOGI(TAG, "restart from the web page");
  delay(300);
  if (hooks.restart) hooks.restart();
  else ESP.restart();
}

void handle_factory_reset() {
  if (!guard()) return;
  send_ok();
  LOGW(TAG, "factory reset from the web page");
  delay(300);
  store::factoryReset();
  ESP.restart();
}

void handle_update_check() {
  if (!guard()) return;
  if (!hooks.check_update || !hooks.check_update()) return send_error(409, "not connected to Harkenn");
  send_ok();
}

void handle_update_install() {
  if (!guard()) return;
  if (!hooks.install_offered || !hooks.install_offered()) return send_error(409, "Harkenn hasn't offered an update");
  send_ok();
}

// Firmware upload: streamed into the spare app slot. Only a Harkenn image for this board is accepted (its marker,
// src/main.cpp), and the bootloader checks the image itself.
struct Upload {
  bool allowed = false, started = false, marker = false;
  String error, version, window;  // window: the tail of what came before, for a marker split across chunks
  size_t size = 0;
} upload;

void scan_marker(const uint8_t* data, size_t len) {
  if (upload.marker) return;
  String text = upload.window;
  text.reserve(text.length() + len);
  for (size_t i = 0; i < len; i++) text += data[i] ? static_cast<char>(data[i]) : '\x01';
  const int at = text.indexOf(MARKER);
  if (at >= 0) {
    const int v = at + strlen(MARKER), h = text.indexOf('|', v), e = h >= 0 ? text.indexOf('|', h + 1) : -1;
    if (e > 0) {
      upload.marker = true;
      upload.version = text.substring(v, h);
      const String hardware = text.substring(h + 1, e);
      if (hardware != HARDWARE_ID) upload.error = "that firmware is for " + hardware + ", not this speaker";
      return;
    }
  }
  upload.window = text.substring(text.length() > 96 ? text.length() - 96 : 0);
}

void handle_upload_data() {
  HTTPUpload& up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    upload = Upload();
    upload.allowed = server->header("X-Requested-With") == "harkenn" && (open_access() || logged_in());
    if (!upload.allowed) return;
    session_used = millis();
    last_use = session_used;
    LOGI(TAG, "firmware upload: %s", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) upload.error = String("can't start: ") + Update.errorString();
    upload.started = !upload.error.length();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upload.started || upload.error.length()) return;
    scan_marker(up.buf, up.currentSize);
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      upload.error = String("flash write failed: ") + Update.errorString();
    }
    upload.size += up.currentSize;
    last_use = millis();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!upload.started) return;
    if (!upload.error.length() && !upload.marker) upload.error = "that isn't Harkenn speaker firmware";
    if (upload.error.length()) {
      Update.abort();
    } else if (!Update.end(true)) {
      upload.error = String("can't finish: ") + Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (upload.started) Update.abort();
    upload.error = "upload interrupted";
  }
}

void handle_upload_done() {
  if (!upload.allowed) return send_error(401, "log in first");
  if (upload.error.length() || !upload.started) {
    LOGW(TAG, "firmware upload failed: %s", upload.error.c_str());
    return send_error(422, upload.error.length() ? upload.error : String("no file"));
  }
  LOGI(TAG, "firmware %s installed from the web page (%u bytes); restarting", upload.version.c_str(),
       static_cast<unsigned>(upload.size));
  store::setUpdatedFrom(FW_VERSION);
  JsonDocument doc;
  doc["ok"] = true;
  doc["version"] = upload.version;
  send_json(200, doc);
  delay(500);
  if (hooks.restart) hooks.restart();
  else ESP.restart();
}

void handle_page() {
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "text/html", PAGE);
}

void handle_not_found() {
  if (hotspot) {  // captive portal: phones probe some URL; send them to the page
    server->sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server->send(302, "text/plain", "");
  } else {
    server->send(404, "text/plain", "not found");
  }
}

void task(void*) {
  while (WiFi.getMode() == WIFI_OFF) vTaskDelay(pdMS_TO_TICKS(200));  // the network stack starts with Wi-Fi
  server = new WebServer(80);
  const char* headers[] = {"Cookie", "X-Requested-With"};
  server->collectHeaders(headers, 2);
  server->on("/", HTTP_GET, handle_page);
  server->on("/api/session", HTTP_GET, handle_session);
  server->on("/api/login", HTTP_POST, handle_login);
  server->on("/api/logout", HTTP_POST, handle_logout);
  server->on("/api/status", HTTP_GET, handle_status);
  server->on("/api/settings", HTTP_POST, handle_settings);
  server->on("/api/wifi", HTTP_GET, handle_wifi_list);
  server->on("/api/wifi", HTTP_POST, handle_wifi_save);
  server->on("/api/wifi/remove", HTTP_POST, handle_wifi_remove);
  server->on("/api/scan", HTTP_GET, handle_scan);
  server->on("/api/logs", HTTP_GET, handle_logs);
  server->on("/api/restart", HTTP_POST, handle_restart);
  server->on("/api/factory-reset", HTTP_POST, handle_factory_reset);
  server->on("/api/update/check", HTTP_POST, handle_update_check);
  server->on("/api/update/install", HTTP_POST, handle_update_install);
  server->on("/api/firmware", HTTP_POST, handle_upload_done, handle_upload_data);
  server->onNotFound(handle_not_found);
  server->begin();
  LOGI(TAG, "web page on port 80");
  for (;;) {
    if (hotspot && !dns) {
      dns = new DNSServer();
      dns->start(53, "*", WiFi.softAPIP());
    } else if (!hotspot && dns) {
      dns->stop();
      delete dns;
      dns = nullptr;
    }
    if (dns) dns->processNextRequest();
    server->handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void begin(const Hooks& h) {
  hooks = h;
  fail_count = store::loginFailures();
  fail_locked = store::loginLocked() || fail_count >= MAX_FAILURES;
  // A restart doesn't skip the wait: it starts again from now.
  const uint32_t w = fail_locked ? 0 : wait_s(fail_count);
  wait_until = w ? millis() + w * 1000 : 0;
  // Internal RAM for the stack: firmware uploads and settings write to flash, which a PSRAM stack can't do.
  xTaskCreatePinnedToCore(task, "web", 8192, nullptr, 1, nullptr, 1);
}

void setHotspot(bool on, bool physical) {
  hotspot_physical = on && physical;
  hotspot = on;
}

bool hotspotOn() { return hotspot; }

bool inUse(uint32_t ms) { return last_use && millis() - last_use < ms; }

void unlock() {
  fail_count = 0;
  fail_locked = false;
  wait_until = 0;
  store::setLoginFailures(0);
  store::setLoginLocked(false);
  LOGI(TAG, "unlocked by Harkenn");
}

uint8_t failures() { return fail_count; }

bool locked() { return fail_locked; }

namespace {

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Harkenn speaker</title>
<style>
:root{--bg:#0e1220;--card:#171c2f;--line:#2a3150;--text:#e6e9f5;--muted:#8d94b3;--accent:#7aa2ff;--bad:#ff6b81;--ok:#5fd3a0}
*{box-sizing:border-box}body{font:15px system-ui,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:16px}
main{max-width:640px;margin:auto}h1{font-size:20px;margin:4px 0 12px}h2{font-size:16px;margin:0 0 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin:12px 0}
label{display:grid;gap:4px;color:var(--muted);margin:8px 0}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.row>*{flex:1}input,select,button{font:inherit;padding:9px;border-radius:8px;border:1px solid var(--line);background:#11162a;color:inherit}
button{background:var(--accent);color:#0b1020;border:0;font-weight:600;cursor:pointer;flex:0 0 auto}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent)}
button.danger{background:transparent;color:var(--bad);border:1px solid var(--bad)}
nav{display:flex;gap:6px;flex-wrap:wrap}nav button{background:transparent;color:var(--muted);border:1px solid var(--line)}
nav button.on{color:var(--text);border-color:var(--accent)}dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:0}
dt{color:var(--muted)}dd{margin:0}.msg{min-height:1.2em;margin-top:8px}.bad{color:var(--bad)}.ok{color:var(--ok)}
.hint{color:var(--muted);font-size:13px}.logs{font:12px ui-monospace,monospace;height:55vh;overflow:auto;white-space:pre-wrap}
.lv-E{color:var(--bad)}.lv-W{color:#ffc46b}.lv-D{color:var(--muted)}.net{display:flex;justify-content:space-between;gap:8px;align-items:center;padding:6px 0;border-bottom:1px solid var(--line)}
[hidden]{display:none!important}
</style></head><body><main>
<h1>Harkenn speaker <span id="name" class="hint"></span></h1>
<section id="login" class="card" hidden><h2>Log in</h2>
<p class="hint">Use the Harkenn admin password or this speaker's own password (shown in Harkenn when it was made).</p>
<form id="login-form"><label>Password<input type="password" id="password" autocomplete="current-password" autofocus></label>
<button>Log in</button><div class="msg" id="login-msg"></div></form></section>
<div id="app" hidden>
<nav><button data-tab="status" class="on">Status</button><button data-tab="wifi">Wi-Fi</button>
<button data-tab="settings">Settings</button><button data-tab="logs">Logs</button><button data-tab="firmware">Firmware</button>
<button data-tab="reset">Restart / reset</button><button id="logout">Log out</button></nav>
<section class="card" data-panel="status"><h2>Status</h2><dl id="status"></dl></section>
<section class="card" data-panel="wifi" hidden><h2>Saved networks</h2><div id="networks"></div>
<h2 style="margin-top:16px">Add or change a network</h2>
<form id="wifi-form"><label>Network<select id="scan"><option value="">Other (type it below)</option></select></label>
<button type="button" class="secondary" id="rescan">Scan again</button>
<label>Network name<input id="ssid" maxlength="32"></label>
<label>Password<input id="wifi-pass" type="password" maxlength="63" autocomplete="new-password" placeholder="empty = open network"></label>
<label class="row"><span><input type="checkbox" id="static" style="width:auto"> Static IP</span></label>
<div id="static-fields" hidden><div class="row"><label>IP address<input id="ip" inputmode="decimal"></label>
<label>Subnet mask<input id="subnet" inputmode="decimal" placeholder="255.255.255.0"></label></div>
<div class="row"><label>Gateway<input id="gateway" inputmode="decimal"></label><label>DNS (optional)<input id="dns" inputmode="decimal"></label></div></div>
<button>Save network</button><div class="msg" id="wifi-msg"></div></form></section>
<section class="card" data-panel="settings" hidden><h2>Settings</h2><form id="settings-form">
<label>Harkenn server address<input id="server" placeholder="empty = find it on the network"></label>
<label>Log level<select id="level"><option>error</option><option>warn</option><option>info</option><option>debug</option></select></label>
<label>Volume <input type="range" id="volume" min="0" max="100"></label>
<label>Brightness <input type="range" id="brightness" min="5" max="100"></label>
<button>Save</button><div class="msg" id="settings-msg"></div>
<p class="hint">Changes are sent to Harkenn when the speaker reaches it, and win over what's set there.</p></form></section>
<section class="card" data-panel="logs" hidden><div class="row"><h2>Logs</h2><label class="row" style="flex:0"><span><input type="checkbox" id="follow" checked style="width:auto"> Follow</span></label></div>
<div class="logs" id="logs"></div></section>
<section class="card" data-panel="firmware" hidden><h2>Firmware</h2><dl id="fw"></dl>
<div class="row" style="margin-top:12px"><button class="secondary" id="check">Ask Harkenn for updates</button><button id="install" hidden>Install</button></div>
<h2 style="margin-top:16px">Upload firmware</h2><p class="hint">A Harkenn speaker firmware.bin for this board. The speaker restarts into it.</p>
<form id="fw-form" class="row"><input type="file" id="file" accept=".bin"><button>Upload</button></form>
<progress id="progress" max="100" value="0" hidden style="width:100%"></progress><div class="msg" id="fw-msg"></div></section>
<section class="card" data-panel="reset" hidden><h2>Restart</h2><button class="secondary" id="restart">Restart the speaker</button>
<h2 style="margin-top:16px">Factory reset</h2><p class="hint">Forgets Wi-Fi, adoption, passwords and settings. The speaker starts over in Wi-Fi setup and has to be adopted again.</p>
<button class="danger" id="reset">Factory reset</button><div class="msg" id="reset-msg"></div></section>
</div></main><script>
"use strict";
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
let tab="status",logSeq=0,status={};
async function api(path,body){
  const opt=body===undefined?{}:{method:"POST",headers:{"X-Requested-With":"harkenn","Content-Type":"application/json"},body:JSON.stringify(body)};
  const r=await fetch(path,opt);let d={};try{d=await r.json()}catch(e){}
  if(r.status==401&&path!="/api/login"){showLogin();}
  if(!r.ok)throw Object.assign(new Error(d.error||("HTTP "+r.status)),{data:d});
  return d;}
function msg(id,text,ok){const m=$(id);m.textContent=text;m.className="msg "+(ok?"ok":"bad");}
function dl(el,rows){el.replaceChildren(...rows.filter(r=>r[1]!==undefined&&r[1]!==null&&r[1]!=="").flatMap(([k,v])=>{const t=document.createElement("dt"),d=document.createElement("dd");t.textContent=k;d.textContent=v;return[t,d]}));}
function showLogin(info){$("#app").hidden=true;$("#login").hidden=false;if(info)msg("#login-msg",info,false);}
async function start(){
  const s=await api("/api/session");
  if(s.open||s.logged_in){$("#login").hidden=true;$("#app").hidden=false;$("#logout").hidden=s.open;refresh();return;}
  showLogin(s.locked?"Locked after too many wrong passwords. Unlock it in Harkenn, or factory reset the speaker (hold its button 11 s).":
    s.wait_s?`Too many wrong passwords: wait ${s.wait_s} s`:!s.passwords?"This speaker hasn't had its passwords from Harkenn yet.":"");}
$("#login-form").onsubmit=async e=>{e.preventDefault();msg("#login-msg","Checking...",true);
  try{await api("/api/login",{password:$("#password").value});$("#password").value="";msg("#login-msg","",true);await start();scan();}catch(err){msg("#login-msg",err.message,false);}};
$("#logout").onclick=async()=>{await api("/api/logout",{});start();};
for(const b of $$("nav [data-tab]"))b.onclick=()=>{tab=b.dataset.tab;for(const x of $$("nav [data-tab]"))x.classList.toggle("on",x==b);
  for(const p of $$("[data-panel]"))p.hidden=p.dataset.panel!=tab;refresh();};
function up(s){const h=Math.floor(s/3600);return h?`${h} h ${Math.floor(s/60)%60} min`:`${Math.floor(s/60)} min`;}
async function refresh(){
  status=await api("/api/status");const s=status;$("#name").textContent=s.name||"";
  if(tab=="status")dl($("#status"),[["Name",s.name],["Harkenn",s.connected?`connected (${s.server})`:s.adopted?"not connected":"not adopted yet"],
    ["Hotspot",s.hotspot?`on: ${s.ap_name}`:undefined],["Wi-Fi",s.ssid?`${s.ssid} (${s.rssi} dBm)`:"not connected"],["IP",s.ip],
    ["State",s.state],["Volume",`${s.volume}%${s.muted?", mic off":""}`],["Brightness",`${s.brightness}%`],
    ["Battery",s.battery!=null&&s.battery>=0?`${s.battery}%${s.charging?", charging":""}`:undefined],["Uptime",up(s.uptime_s)],
    ["Free memory",`${Math.round(s.free_heap/1024)} KB internal, ${Math.round(s.free_psram/1024)} KB PSRAM`],
    ["Firmware",s.firmware],["Speaker",s.device_id],["Not yet sent to Harkenn",s.local_changes?"some changes":undefined]]);
  if(tab=="wifi")loadWifi();
  if(tab=="settings"&&!$("#settings-form").contains(document.activeElement)){$("#server").value=s.server_setting||"";$("#level").value=s.log_level;
    $("#volume").value=s.volume;$("#brightness").value=s.brightness;}
  if(tab=="firmware"){dl($("#fw"),[["Installed",s.firmware],["Board",s.hardware],["Offered by Harkenn",s.update_offered]]);$("#install").hidden=!s.update_offered;}
  if(tab=="logs")loadLogs();}
async function loadWifi(){const w=await api("/api/wifi");const box=$("#networks");box.replaceChildren();
  if(!w.networks.length)box.textContent="None yet.";
  for(const n of w.networks){const row=document.createElement("div");row.className="net";const t=document.createElement("span");
    t.textContent=`${n.ssid}${n.ssid==w.current?" (connected)":""}${n.open?" · open":""}${n.ip?" · static "+n.ip:""}`;
    const edit=document.createElement("button");edit.className="secondary";edit.textContent="Edit";edit.onclick=()=>{$("#ssid").value=n.ssid;$("#wifi-pass").value="";
      $("#wifi-pass").placeholder="leave empty to keep the saved password";$("#static").checked=!!n.ip;$("#static-fields").hidden=!n.ip;
      for(const k of["ip","subnet","gateway","dns"])$("#"+k).value=n[k]||"";};
    const rm=document.createElement("button");rm.className="danger";rm.textContent="Remove";rm.onclick=async()=>{
      if(!confirm(`Forget ${n.ssid}?`))return;try{await api("/api/wifi/remove",{ssid:n.ssid});loadWifi();}catch(e){msg("#wifi-msg",e.message,false);}};
    const b=document.createElement("span");b.className="row";b.style.flex="0";b.append(edit,rm);row.append(t,b);box.append(row);}}
async function scan(){const sel=$("#scan");sel.options[0].textContent="Scanning...";
  try{const r=await api("/api/scan");sel.replaceChildren(new Option("Other (type it below)",""),...r.networks.map(n=>new Option(`${n.ssid} (${n.rssi} dBm${n.open?", open":""})`,n.ssid)));}
  catch(e){sel.options[0].textContent="Other (type it below)";msg("#wifi-msg",e.message,false);}}
$("#scan").onchange=()=>{if($("#scan").value){$("#ssid").value=$("#scan").value;$("#wifi-pass").placeholder="empty = open network";}};
$("#rescan").onclick=scan;$("#static").onchange=()=>{$("#static-fields").hidden=!$("#static").checked;};
$("#wifi-form").onsubmit=async e=>{e.preventDefault();const body={ssid:$("#ssid").value.trim()};
  const pw=$("#wifi-pass").value;if(pw||!$("#wifi-pass").placeholder.startsWith("leave"))body.password=pw;
  for(const k of["ip","subnet","gateway","dns"])body[k]=$("#static").checked?$("#"+k).value.trim():"";
  try{await api("/api/wifi",body);msg("#wifi-msg",`Saved ${body.ssid}. The speaker tries it now.`,true);$("#wifi-pass").value="";loadWifi();}catch(err){msg("#wifi-msg",err.message,false);}};
$("#settings-form").onsubmit=async e=>{e.preventDefault();
  try{await api("/api/settings",{server_url:$("#server").value.trim(),log_level:$("#level").value,volume:+$("#volume").value,brightness:+$("#brightness").value});
    msg("#settings-msg","Saved",true);}catch(err){msg("#settings-msg",err.message,false);}};
async function loadLogs(){const r=await api(`/api/logs?after=${logSeq}`);if(r.newest<logSeq){logSeq=0;$("#logs").replaceChildren();return loadLogs();}
  const box=$("#logs");for(const l of r.lines){const d=document.createElement("div");d.className="lv-"+l.level;
    const when=l.ts?new Date(l.ts*1000).toLocaleTimeString():`+${(l.up_ms/1000).toFixed(1)} s`;d.textContent=`${when} ${l.level} ${l.tag}: ${l.message}`;box.append(d);}
  logSeq=r.newest;while(box.childElementCount>1000)box.firstElementChild.remove();if($("#follow").checked)box.scrollTop=box.scrollHeight;}
$("#check").onclick=async()=>{try{await api("/api/update/check",{});msg("#fw-msg","Asked Harkenn: an offer shows here and on the screen.",true);setTimeout(refresh,3000);}catch(e){msg("#fw-msg",e.message,false);}};
$("#install").onclick=async()=>{try{await api("/api/update/install",{});msg("#fw-msg","Installing: watch the speaker's screen.",true);}catch(e){msg("#fw-msg",e.message,false);}};
$("#fw-form").onsubmit=e=>{e.preventDefault();const f=$("#file").files[0];if(!f){msg("#fw-msg","Choose a firmware file first",false);return;}
  const x=new XMLHttpRequest(),form=new FormData();form.append("firmware",f);x.open("POST","/api/firmware");x.setRequestHeader("X-Requested-With","harkenn");
  const p=$("#progress");p.hidden=false;x.upload.onprogress=ev=>{if(ev.lengthComputable)p.value=ev.loaded*100/ev.total;};
  x.onload=()=>{let d={};try{d=JSON.parse(x.responseText)}catch(e){}p.hidden=true;
    if(x.status==200)msg("#fw-msg",`Installed ${d.version}. The speaker is restarting.`,true);else msg("#fw-msg",d.error||("HTTP "+x.status),false);};
  x.onerror=()=>{p.hidden=true;msg("#fw-msg","Upload failed",false);};msg("#fw-msg","Uploading...",true);x.send(form);};
$("#restart").onclick=async()=>{if(!confirm("Restart the speaker?"))return;try{await api("/api/restart",{});msg("#reset-msg","Restarting...",true);}catch(e){msg("#reset-msg",e.message,false);}};
$("#reset").onclick=async()=>{if(!confirm("Factory reset? The speaker forgets everything and has to be set up and adopted again."))return;
  try{await api("/api/factory-reset",{});msg("#reset-msg","Resetting. The speaker starts over in Wi-Fi setup.",true);}catch(e){msg("#reset-msg",e.message,false);}};
start().then(()=>{if(!$("#app").hidden)scan();}).catch(e=>showLogin(e.message));
setInterval(()=>{if(!$("#app").hidden)refresh().catch(()=>{});},4000);
</script></body></html>)HTML";

}  // namespace
}  // namespace web
