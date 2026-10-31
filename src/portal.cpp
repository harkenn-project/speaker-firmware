// Wi-Fi setup portal: an open access point with a captive page for choosing the network (and, if needed, the
// Kotodama server URL). Saves to NVS and restarts.
#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "logbuf.h"
#include "store.h"
#include "ui.h"

namespace {

String html_escape(const String& s) {
  String out;
  out.reserve(s.length());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Kotodama speaker setup</title>
<style>body{font:16px system-ui,sans-serif;background:#0e1220;color:#e6e9f5;margin:0;padding:24px}
form{max-width:420px;margin:auto;display:grid;gap:14px}label{display:grid;gap:4px;color:#8d94b3}
input,select,button{font:inherit;padding:10px;border-radius:8px;border:1px solid #2a3150;background:#171c2f;color:inherit}
button{background:#7aa2ff;color:#0b1020;border:0;font-weight:600}details{color:#8d94b3}</style></head><body>)";

}  // namespace

namespace portal {

void run(const String& apName) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  const IPAddress ip = WiFi.softAPIP();
  LOGI("portal", "access point %s at %s", apName.c_str(), ip.toString().c_str());
  ui::showWifiSetup(apName.c_str());

  const int found = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length() || options.indexOf(">" + html_escape(ssid) + " (") >= 0) continue;
    options += "<option value=\"" + html_escape(ssid) + "\">" + html_escape(ssid) + " (" + WiFi.RSSI(i) + " dBm)</option>";
  }

  DNSServer dns;
  dns.start(53, "*", ip);
  WebServer server(80);

  server.on("/", HTTP_GET, [&]() {
    String page = FPSTR(PAGE_HEAD);
    page += "<form method=post action=/save><h2>Kotodama speaker</h2>";
    page += "<label>Network<select name=ssid><option value=''>Other (type below)</option>" + options + "</select></label>";
    page += "<label>Other network name<input name=other maxlength=32></label>";
    page += "<label>Password<input name=password type=password maxlength=64></label>";
    page += "<details><summary>Advanced</summary><label>Kotodama server<input name=server value=\"" +
            html_escape(store::serverUrl()) + "\"></label></details>";
    page += "<button>Save and connect</button></form></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/save", HTTP_POST, [&]() {
    String ssid = server.arg("ssid");
    if (!ssid.length()) ssid = server.arg("other");
    ssid.trim();
    const String password = server.arg("password");
    String serverUrl = server.arg("server");
    serverUrl.trim();
    if (!ssid.length() || ssid.length() > 32 || (password.length() && password.length() < 8)) {
      server.send(400, "text/html", String(FPSTR(PAGE_HEAD)) +
                                        "<p>Pick a network, and use a password of at least 8 characters (or none for "
                                        "an open network). <a href=/>Back</a></p>");
      return;
    }
    store::upsertWifi(ssid, password);
    if (serverUrl.startsWith("http://") || serverUrl.startsWith("https://")) store::setServerUrl(serverUrl);
    LOGI("portal", "saved network %s", ssid.c_str());
    server.send(200, "text/html", String(FPSTR(PAGE_HEAD)) + "<p>Saved. The speaker is restarting and will join " +
                                      html_escape(ssid) + ".</p></body></html>");
    delay(1500);
    ESP.restart();
  });

  // Captive portal: send every other URL to the form.
  server.onNotFound([&]() {
    server.sendHeader("Location", "http://" + ip.toString() + "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}

}  // namespace portal
