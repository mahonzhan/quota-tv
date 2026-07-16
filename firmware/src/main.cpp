// ============================================================
// QuotaTV — Claude Code + ChatGPT(Codex) 订阅额度桌面显示器
// 硬件: ESP32-C3 DevKitM-1 + 1.8" ST7735 (128x160) + 低电平有源蜂鸣器
// 功能: 单屏显示两家 5h/周额度利用率 + 重置倒计时, 额度重置时蜂鸣提醒
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "config.h"

// ---------------- 全局 ----------------
TFT_eSPI    tft;
TFT_eSprite canvas(&tft);          // 全屏离屏缓冲, 防闪烁 (160x128x2 = 40KB)
Preferences prefs;
WebServer   server(80);
DNSServer   dns;
WiFiUDP     udp;                   // UDP 发现响应 (不依赖 mDNS)
String      devName;               // quotatv-xxxx (MAC 后缀, 全网唯一)
#define DISCOVERY_PORT 18324

struct Cfg {
  String ssid, pass;
  String mode;                     // "direct" 直连 (默认) / "agent" 主机推送
  String claudeToken;              // sk-ant-oat01-... (claude setup-token)
  String codexAccess, codexRefresh;
} cfg;

struct WindowData {
  float  usedPct    = -1;          // 0~100, <0 = 无数据
  time_t resetAt    = 0;           // unix epoch, 0 = 未知
};
uint32_t staleAfterMs = STALE_AFTER_MS;   // agent 模式下由推送帧 interval 动态覆盖
struct ProviderData {
  WindowData session, weekly;      // 5h / 7d
  uint32_t lastOkMs   = 0;
  String   lastErr;
  bool fresh() const { return lastOkMs && (millis() - lastOkMs) < staleAfterMs; }
  uint32_t ageMin() const { return lastOkMs ? (millis() - lastOkMs) / 60000UL : 0; }
};
ProviderData claudeData, codexData;

bool portalMode = false;
uint32_t lastPollMs = 0;
bool firstPollDone = false;

// ---------------- 颜色 ----------------
#define C_BG        0x0000                       // 黑
#define C_HEADER    0x39C7                       // 深灰
#define C_TEXT      0xEF7D                       // 近白
#define C_DIM       0x8410                       // 中灰
#define C_CLAUDE    0xD3AB                       // Claude 品牌橙红 #D97757 精确转换
#define C_CODEX     0xFFFF                       // Codex 白
#define C_BAR_BG    0x2124                       // bar 底槽
#define C_WARN      0xFDA0                       // 黄
#define C_DANGER    0xF9E7                       // 红

// ============================================================
// 蜂鸣器 (低电平触发)
// ============================================================
static void buzzInit() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH);  // 立刻拉高静音
}
static bool inQuietHours() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return false;
  int h = t.tm_hour;
  if (QUIET_HOUR_START > QUIET_HOUR_END)       // 跨午夜, 如 23~8
    return h >= QUIET_HOUR_START || h < QUIET_HOUR_END;
  return h >= QUIET_HOUR_START && h < QUIET_HOUR_END;
}
static void beep(uint8_t n, uint16_t onMs = 90, uint16_t offMs = 90) {
  for (uint8_t i = 0; i < n; i++) {
    digitalWrite(PIN_BUZZER, LOW);  delay(onMs);
    digitalWrite(PIN_BUZZER, HIGH); delay(offMs);
  }
}

// ============================================================
// 小工具
// ============================================================
static String fmtCountdown(time_t resetAt) {
  time_t now = time(nullptr);
  if (resetAt <= 0 || now < 1600000000 || resetAt <= now) return "";
  long s = resetAt - now;
  char buf[16];
  if (s >= 86400)      snprintf(buf, sizeof(buf), "%ldd%02ldh", s / 86400, (s % 86400) / 3600);
  else if (s >= 3600)  snprintf(buf, sizeof(buf), "%ldh%02ldm", s / 3600, (s % 3600) / 60);
  else                 snprintf(buf, sizeof(buf), "%ldm", s / 60);
  return String(buf);
}

// base64url 解码 (用于解析 JWT exp)
static String b64urlDecode(const String& in) {
  String s = in;
  s.replace('-', '+'); s.replace('_', '/');
  while (s.length() % 4) s += '=';
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out; out.reserve(s.length() * 3 / 4);
  int val = 0, bits = 0;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '=') break;
    const char* p = strchr(T, c);
    if (!p) continue;
    val = (val << 6) | (p - T);
    bits += 6;
    if (bits >= 8) { bits -= 8; out += (char)((val >> bits) & 0xFF); }
  }
  return out;
}
static time_t jwtExp(const String& jwt) {
  int p1 = jwt.indexOf('.'), p2 = jwt.indexOf('.', p1 + 1);
  if (p1 < 0 || p2 < 0) return 0;
  String payload = b64urlDecode(jwt.substring(p1 + 1, p2));
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return 0;
  return (time_t)(doc["exp"] | 0UL);
}

// ============================================================
// 重置检测 + 蜂鸣
// ============================================================
static void checkReset(const char* name, WindowData& prev, const WindowData& fresh_, uint8_t beeps) {
  bool wasValid = prev.usedPct >= 0;
  bool dropped  = wasValid && fresh_.usedPct >= 0 &&
                  (prev.usedPct - fresh_.usedPct) >= RESET_DROP_THRESHOLD;
  bool resetPassed = prev.resetAt > 0 && fresh_.resetAt > prev.resetAt &&
                     time(nullptr) >= prev.resetAt && wasValid && prev.usedPct >= 20;
  if (dropped || resetPassed) {
    Serial.printf("[beep] %s reset: %.0f%% -> %.0f%%\n", name, prev.usedPct, fresh_.usedPct);
    if (!inQuietHours()) beep(beeps);
  }
}

// ============================================================
// Claude: 最小请求, 读 anthropic-ratelimit-unified-* 响应头
// ============================================================
static bool fetchClaude() {
  if (cfg.claudeToken.isEmpty()) { claudeData.lastErr = "no token"; return false; }
  WiFiClientSecure client;
  client.setInsecure();                      // 简化: 不校验证书 (README 有说明)
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String("https://") + CLAUDE_API_HOST + CLAUDE_API_PATH)) {
    claudeData.lastErr = "begin fail"; return false;
  }
  const char* keys[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-reset",
  };
  http.collectHeaders(keys, 4);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + cfg.claudeToken);
  http.addHeader("anthropic-version", "2023-06-01");
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  String body = String("{\"model\":\"") + CLAUDE_PROBE_MODEL +
                "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
  int code = http.POST(body);
  String u5 = http.header(keys[0]), u7 = http.header(keys[1]);
  String r5 = http.header(keys[2]), r7 = http.header(keys[3]);
  http.end();

  if (u5.isEmpty() && u7.isEmpty()) {
    claudeData.lastErr = "HTTP " + String(code);
    Serial.printf("[claude] no ratelimit headers, code=%d\n", code);
    return false;
  }
  WindowData s, w;
  if (!u5.isEmpty()) s.usedPct = u5.toFloat() * 100.0f;
  if (!u7.isEmpty()) w.usedPct = u7.toFloat() * 100.0f;
  // reset 头可能是 unix epoch 或 RFC3339, 只处理纯数字情况
  if (r5.length() && r5[0] >= '0' && r5[0] <= '9' && r5.indexOf('-') < 0) s.resetAt = (time_t)strtoul(r5.c_str(), nullptr, 10);
  if (r7.length() && r7[0] >= '0' && r7[0] <= '9' && r7.indexOf('-') < 0) w.resetAt = (time_t)strtoul(r7.c_str(), nullptr, 10);

  checkReset("claude-5h", claudeData.session, s, 2);
  checkReset("claude-7d", claudeData.weekly,  w, 2);
  claudeData.session = s; claudeData.weekly = w;
  claudeData.lastOkMs = millis(); claudeData.lastErr = "";
  Serial.printf("[claude] 5h=%.1f%% 7d=%.1f%%\n", s.usedPct, w.usedPct);
  return true;
}

// ============================================================
// Codex: GET wham/usage, 必要时用 refresh token 刷新
// ============================================================
static bool codexRefreshToken() {
  if (cfg.codexRefresh.isEmpty()) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, OPENAI_TOKEN_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument req;
  req["client_id"]     = CODEX_CLIENT_ID;
  req["grant_type"]    = "refresh_token";
  req["refresh_token"] = cfg.codexRefresh;
  req["scope"]         = "openid profile email";
  String body; serializeJson(req, body);
  int code = http.POST(body);
  if (code != 200) { http.end(); Serial.printf("[codex] refresh fail %d\n", code); return false; }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) return false;
  const char* at = doc["access_token"];
  const char* rt = doc["refresh_token"];
  if (!at) return false;
  cfg.codexAccess = at;
  if (rt && strlen(rt)) cfg.codexRefresh = rt;
  prefs.begin("quotatv", false);
  prefs.putString("cx_at", cfg.codexAccess);
  prefs.putString("cx_rt", cfg.codexRefresh);
  prefs.end();
  Serial.println("[codex] token refreshed");
  return true;
}

// 从 wham/usage 的 window 对象里尽量兼容地取值
static void parseCodexWindow(JsonVariantConst w, WindowData& out) {
  if (w.isNull()) return;
  if (w["used_percent"].is<float>())        out.usedPct = w["used_percent"].as<float>();
  else if (w["usage_percent"].is<float>())  out.usedPct = w["usage_percent"].as<float>();
  else if (w["used"].is<float>())           out.usedPct = w["used"].as<float>();
  time_t now = time(nullptr);
  if (w["resets_at"].is<long long>())            out.resetAt = (time_t)w["resets_at"].as<long long>();
  else if (w["reset_at"].is<long long>())        out.resetAt = (time_t)w["reset_at"].as<long long>();
  else if (w["resets_in_seconds"].is<long>())    out.resetAt = now + w["resets_in_seconds"].as<long>();
  else if (w["reset_after_seconds"].is<long>())  out.resetAt = now + w["reset_after_seconds"].as<long>();
  if (out.resetAt > 4102444800LL) out.resetAt /= 1000;   // 毫秒时间戳兜底
}

static bool fetchCodex() {
  if (cfg.codexAccess.isEmpty()) { codexData.lastErr = "no token"; return false; }
  // 到期前主动刷新
  time_t exp = jwtExp(cfg.codexAccess);
  time_t now = time(nullptr);
  if (exp > 0 && now > 1600000000 && exp - now < (time_t)CODEX_REFRESH_MARGIN_S) codexRefreshToken();

  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, CODEX_USAGE_URL)) { codexData.lastErr = "begin fail"; return false; }
    http.addHeader("Authorization", "Bearer " + cfg.codexAccess);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "QuotaTV/1.0");
    int code = http.GET();
    if (code == 401 && attempt == 0) {           // 过期 → 刷新重试
      http.end();
      if (!codexRefreshToken()) { codexData.lastErr = "401 refresh fail"; return false; }
      continue;
    }
    if (code != 200) { http.end(); codexData.lastErr = "HTTP " + String(code); return false; }
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, http.getStream());
    http.end();
    if (e) { codexData.lastErr = "json err"; return false; }

    JsonVariantConst rl = doc["rate_limit"];
    if (rl.isNull()) rl = doc["rate_limits"];
    WindowData s, w;
    parseCodexWindow(rl["primary_window"],   s);
    parseCodexWindow(rl["secondary_window"], w);
    if (s.usedPct < 0 && w.usedPct < 0) {
      // 结构不认识: 打印首 300 字符方便排查
      String dump; serializeJson(doc, dump);
      Serial.println("[codex] unknown shape: " + dump.substring(0, 300));
      codexData.lastErr = "parse shape";
      return false;
    }
    checkReset("codex-5h", codexData.session, s, 3);
    checkReset("codex-7d", codexData.weekly,  w, 3);
    codexData.session = s; codexData.weekly = w;
    codexData.lastOkMs = millis(); codexData.lastErr = "";
    Serial.printf("[codex] 5h=%.1f%% 7d=%.1f%%\n", s.usedPct, w.usedPct);
    return true;
  }
  return false;
}

// ============================================================
// UI (160x128 横屏)
// ============================================================
static uint16_t barColor(float pct) {
  if (pct >= 90) return C_DANGER;
  if (pct >= 70) return C_WARN;
  return C_TEXT;
}

static void drawBarRow(int y, const char* label, const WindowData& w, uint16_t accent) {
  canvas.setTextFont(1);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(TL_DATUM);
  canvas.drawString(label, 6, y + 1);

  const int bx = 26, bw = 86, bh = 9;
  canvas.fillRoundRect(bx, y, bw, bh, 2, C_BAR_BG);
  if (w.usedPct >= 0) {
    int fw = (int)(bw * constrain(w.usedPct, 0.0f, 100.0f) / 100.0f);
    if (fw > 0) canvas.fillRoundRect(bx, y, max(fw, 3), bh, 2, accent);
    // 百分比 (整组已变灰时百分比也灰)
    canvas.setTextColor(accent == C_DIM ? C_DIM : barColor(w.usedPct), C_BG);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(String((int)(w.usedPct + 0.5f)) + "%", 154, y + 1);
  } else {
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString("--", 154, y + 1);
  }
}

static void drawProvider(int y, const char* name, uint16_t accent, const ProviderData& d) {
  bool stale = !d.fresh();
  uint16_t ac = stale ? C_DIM : accent;      // stale: 整组变灰
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(ac, C_BG);
  canvas.drawString(name, 6, y);
  // 右侧: stale 时显示距上次成功多久, 否则显示倒计时
  String cd = fmtCountdown(d.session.resetAt);
  canvas.setTextColor(stale ? C_WARN : C_DIM, C_BG);
  canvas.setTextDatum(TR_DATUM);
  if (stale) {
    String msg = d.lastOkMs ? ("stale " + String(d.ageMin()) + "m")
                            : (d.lastErr.length() ? d.lastErr : "no data");
    canvas.drawString(msg, 154, y);
  } else if (cd.length()) {
    canvas.drawString("5h reset " + cd, 154, y);
  }
  drawBarRow(y + 12, "5H", d.session, ac);
  drawBarRow(y + 25, "7D", d.weekly,  ac);
}

static void drawScreen() {
  canvas.fillSprite(C_BG);
  // 头部
  canvas.fillRect(0, 0, 160, 13, C_HEADER);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(C_TEXT, C_HEADER);
  canvas.drawString("QUOTA TV", 6, 3);
  struct tm t;
  if (getLocalTime(&t, 20)) {
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(hm, 132, 3);
  }
  // WiFi 状态点
  canvas.fillCircle(148, 6, 3, WiFi.status() == WL_CONNECTED ? 0x07E0 : C_DANGER);

  drawProvider(20, "CLAUDE", C_CLAUDE, claudeData);
  canvas.drawFastHLine(6, 70, 148, C_HEADER);
  drawProvider(78, "CODEX", C_CODEX, codexData);

  // 底部状态行
  canvas.setTextDatum(BL_DATUM);
  canvas.setTextColor(C_DIM, C_BG);
  // 底行交替显示设备名和 IP (各 4 秒)
  bool showName = (millis() / 4000) % 2 == 0;
  String bottom = showName ? devName : WiFi.localIP().toString();
  if (cfg.mode == "agent" && !firstPollDone) bottom = devName + " wait push";
  else if (!firstPollDone) bottom = "loading...";
  canvas.drawString(bottom, 6, 127);
  canvas.pushSprite(0, 0);
}

static void drawMessage(const char* l1, const char* l2 = "", const char* l3 = "") {
  canvas.fillSprite(C_BG);
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(C_CLAUDE, C_BG); canvas.drawString("QUOTA TV", 6, 6);
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(l1, 6, 34);
  canvas.drawString(l2, 6, 50);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString(l3, 6, 66);
  canvas.pushSprite(0, 0);
}

// ============================================================
// 配置门户 (AP + captive portal)
// ============================================================
static const char PORTAL_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>QuotaTV Setup</title><style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 14px;background:#111;color:#eee}
h2{color:#D97757}label{display:block;margin:12px 0 4px;font-size:14px;color:#aaa}
input,textarea{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;font-size:13px}
textarea{height:64px}button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:#D97757;color:#fff;font-size:16px}
small{color:#777}</style></head><body>
<h2>QuotaTV 配置</h2>
<form method="POST" action="/save">
<label>WiFi 名称 (2.4GHz)</label><input name="ssid" required>
<label>WiFi 密码</label><input name="pass" type="password">
<label>工作模式</label>
<select name="mode" style="width:100%;padding:8px;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee">
<option value="direct">直连模式 — 设备自己拉数据 (需填下方 token)</option>
<option value="agent">主机 Agent 模式 — 电脑推送 (token 留空)</option>
</select>
<label>Claude OAuth Token <small>(终端运行 claude setup-token 获取, sk-ant-oat01-...)</small></label>
<textarea name="ctok"></textarea>
<label>Codex Access Token <small>(~/.codex/auth.json 的 tokens.access_token)</small></label>
<textarea name="cxat"></textarea>
<label>Codex Refresh Token <small>(tokens.refresh_token, 用于自动续期)</small></label>
<textarea name="cxrt"></textarea>
<button type="submit">保存并重启</button>
</form>
<p><small>Agent 模式: 到 github.com/mahonzhan/quota-tv/releases 下载对应平台的
quota-agent 桌面应用, 在电脑上常驻运行即可, 无需在此填 token。<br>
直连模式: 三个 token 可用仓库 tools/get_tokens.py 一键打印。</small></p>
</body></html>)html";

static void handleSave() {
  prefs.begin("quotatv", false);
  prefs.putString("ssid",  server.arg("ssid"));
  prefs.putString("pass",  server.arg("pass"));
  prefs.putString("mode",  server.arg("mode") == "agent" ? "agent" : "direct");
  if (server.arg("ctok").length()) prefs.putString("c_tok", server.arg("ctok"));
  if (server.arg("cxat").length()) prefs.putString("cx_at", server.arg("cxat"));
  if (server.arg("cxrt").length()) prefs.putString("cx_rt", server.arg("cxrt"));
  prefs.end();
  server.send(200, "text/html; charset=utf-8",
              "<h3>已保存, 设备重启中...</h3>");
  delay(800);
  ESP.restart();
}

static void startPortal() {
  portalMode = true;
  String apName = devName;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", AP_IP);
  server.onNotFound([]() { server.send(200, "text/html; charset=utf-8", FPSTR(PORTAL_HTML)); });
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  drawMessage("Setup mode", ("WiFi: " + apName).c_str(), "open 192.168.4.1");
  Serial.printf("[portal] AP %s, http://192.168.4.1\n", apName.c_str());
}

// Agent 推送: 把 JSON 里的一个 provider 应用到数据结构
static void applyPush(JsonVariantConst v, ProviderData& d, const char* name, uint8_t beeps) {
  if (v.isNull()) return;
  WindowData s = d.session, w = d.weekly;
  if (v["session"].is<float>())      s.usedPct = v["session"].as<float>();
  if (v["weekly"].is<float>())       w.usedPct = v["weekly"].as<float>();
  if (v["sessionReset"].is<long long>()) s.resetAt = (time_t)v["sessionReset"].as<long long>();
  if (v["weeklyReset"].is<long long>())  w.resetAt = (time_t)v["weeklyReset"].as<long long>();
  checkReset((String(name) + "-5h").c_str(), d.session, s, beeps);
  checkReset((String(name) + "-7d").c_str(), d.weekly,  w, beeps);
  d.session = s; d.weekly = w;
  d.lastOkMs = millis(); d.lastErr = "";
}

// STA 模式下的辅助端点
static void startStaServer() {
  // 主机 Agent 推送入口:
  // POST /push {"claude":{"session":62,"weekly":41,"sessionReset":1789e6,"weeklyReset":...},"codex":{...}}
  server.on("/push", HTTP_POST, []() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"err\":\"bad json\"}");
      return;
    }
    applyPush(doc["claude"], claudeData, "claude", 2);
    applyPush(doc["codex"],  codexData,  "codex",  3);
    // Agent 告知自己的推送间隔(秒) → stale 阈值 = 3x 实际周期
    long iv = doc["interval"] | 0L;
    if (iv >= 10 && iv <= 3600) staleAfterMs = (uint32_t)iv * 3000UL;
    firstPollDone = true;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("[push] frame accepted");
  });
  server.on("/", []() {
    String s = "QuotaTV\nclaude 5h=" + String(claudeData.session.usedPct) +
               " 7d=" + String(claudeData.weekly.usedPct) +
               "\ncodex  5h=" + String(codexData.session.usedPct) +
               " 7d=" + String(codexData.weekly.usedPct) + "\n";
    server.send(200, "text/plain", s);
  });
  server.on("/status", []() {
    JsonDocument doc;
    doc["name"]              = devName;
    doc["mode"]              = cfg.mode;
    doc["claude"]["session"] = claudeData.session.usedPct;
    doc["claude"]["weekly"]  = claudeData.weekly.usedPct;
    doc["claude"]["err"]     = claudeData.lastErr;
    doc["codex"]["session"]  = codexData.session.usedPct;
    doc["codex"]["weekly"]   = codexData.weekly.usedPct;
    doc["codex"]["err"]      = codexData.lastErr;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/portal", HTTP_POST, []() {      // 重启进配置模式 (保留已有配置)
    prefs.begin("quotatv", false); prefs.putBool("portal", true); prefs.end();
    server.send(200, "text/plain", "rebooting into setup mode");
    delay(400); ESP.restart();
  });
  server.on("/reset", HTTP_POST, []() {       // 清空全部配置
    prefs.begin("quotatv", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "cleared, rebooting");
    delay(500); ESP.restart();
  });
  server.begin();
}

// ============================================================
// setup / loop
// ============================================================
static void loadCfg() {
  prefs.begin("quotatv", true);
  cfg.ssid         = prefs.getString("ssid", "");
  cfg.pass         = prefs.getString("pass", "");
  cfg.mode         = prefs.getString("mode", "direct");
  cfg.claudeToken  = prefs.getString("c_tok", "");
  cfg.codexAccess  = prefs.getString("cx_at", "");
  cfg.codexRefresh = prefs.getString("cx_rt", "");
  prefs.end();
}

void setup() {
  buzzInit();                                  // 最先: 低电平触发, 避免上电长鸣
  Serial.begin(115200);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  devName = String("quotatv-") + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);

  pinMode(PIN_TFT_BL, OUTPUT);
  analogWrite(PIN_TFT_BL, 200);                // 背光 ~80%

  tft.init();
  tft.setRotation(3);                          // 横屏 160x128; 方向反了改成 1
  canvas.setColorDepth(16);
  canvas.createSprite(160, 128);

  loadCfg();

  // 配置模式触发: 运行中长按 BOOT 3 秒 (写标记后重启), 或 POST /portal
  // 注意: 不能用"按住 BOOT 上电"—— C3 的 GPIO9 上电为低会进串口下载模式, 固件不运行
  prefs.begin("quotatv", false);
  bool portalFlag = prefs.getBool("portal", false);
  if (portalFlag) prefs.putBool("portal", false);
  prefs.end();

  if (cfg.ssid.isEmpty() || portalFlag) { startPortal(); return; }

  drawMessage("Connecting WiFi", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    // 连不上 (密码错/换网络) → 直接进配置模式, 不再重启循环
    drawMessage("WiFi failed", "entering setup...");
    delay(1500);
    startPortal();
    return;
  }

  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);
  drawMessage("Syncing time...");
  struct tm t;
  getLocalTime(&t, 8000);

  // 唯一主机名注册 mDNS: http://quotatv-xxxx.local (多设备不冲突)
  if (MDNS.begin(devName.c_str())) MDNS.addService("http", "tcp", 80);
  udp.begin(DISCOVERY_PORT);                   // UDP 广播发现 (mDNS 不可用时的主路径)

  startStaServer();
  beep(1, 60);                                 // 就绪提示
  if (cfg.mode == "agent") firstPollDone = false;  // 等待 Agent 首帧
  drawScreen();
}

void loop() {
  if (portalMode) { dns.processNextRequest(); server.handleClient(); return; }

  server.handleClient();

  // UDP 发现: 收到 "QUOTATV_DISCOVER" 即回自己的名字+IP
  if (udp.parsePacket() > 0) {
    char buf[40];
    int n = udp.read(buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    if (strncmp(buf, "QUOTATV_DISCOVER", 16) == 0) {
      String r = "{\"name\":\"" + devName + "\",\"ip\":\"" +
                 WiFi.localIP().toString() + "\",\"mode\":\"" + cfg.mode + "\"}";
      udp.beginPacket(udp.remoteIP(), udp.remotePort());
      udp.print(r);
      udp.endPacket();
    }
  }

  uint32_t nowMs = millis();
  if (cfg.mode != "agent" &&
      (!firstPollDone || nowMs - lastPollMs >= POLL_INTERVAL_MS)) {
    lastPollMs = nowMs;
    fetchClaude();
    fetchCodex();
    firstPollDone = true;
    drawScreen();
  }

  // 每秒刷新一次 (时钟/倒计时)
  static uint32_t lastDraw = 0;
  if (nowMs - lastDraw >= 1000) { lastDraw = nowMs; drawScreen(); }

  // 运行中 BOOT 键: 短按=立即刷新, 长按 3 秒(蜂鸣)=重启进配置模式
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    uint32_t press = millis();
    bool longPress = false;
    while (digitalRead(PIN_BOOT_BTN) == LOW) {
      if (!longPress && millis() - press >= 3000) {
        longPress = true;
        beep(1, 250);                       // 提示: 可以松手了
      }
      delay(10);
    }
    if (longPress) {
      prefs.begin("quotatv", false);
      prefs.putBool("portal", true);
      prefs.end();
      ESP.restart();
    } else if (millis() - press >= 50) {
      lastPollMs = 0; firstPollDone = false; // 短按强制刷新
    }
  }
  delay(10);
}
