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

struct Cfg {
  String ssid, pass;
  String claudeToken;              // sk-ant-oat01-... (claude setup-token)
  String codexAccess, codexRefresh;
} cfg;

struct WindowData {
  float  usedPct    = -1;          // 0~100, <0 = 无数据
  time_t resetAt    = 0;           // unix epoch, 0 = 未知
};
struct ProviderData {
  WindowData session, weekly;      // 5h / 7d
  uint32_t lastOkMs   = 0;
  String   lastErr;
  bool fresh() const { return lastOkMs && (millis() - lastOkMs) < STALE_AFTER_MS; }
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
    // 百分比
    canvas.setTextColor(barColor(w.usedPct), C_BG);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(String((int)(w.usedPct + 0.5f)) + "%", 154, y + 1);
  } else {
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString("--", 154, y + 1);
  }
}

static void drawProvider(int y, const char* name, uint16_t accent, const ProviderData& d) {
  canvas.setTextFont(1);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(accent, C_BG);
  canvas.drawString(name, 6, y);
  // 倒计时 (取 5h 窗口)
  String cd = fmtCountdown(d.session.resetAt);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.setTextDatum(TR_DATUM);
  if (!d.fresh())            canvas.drawString(d.lastErr.length() ? d.lastErr : "stale", 154, y);
  else if (cd.length())      canvas.drawString("5h reset " + cd, 154, y);
  drawBarRow(y + 12, "5H", d.session, accent);
  drawBarRow(y + 25, "7D", d.weekly,  accent);
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
  if (!firstPollDone) canvas.drawString("loading...", 6, 127);
  else canvas.drawString(WiFi.localIP().toString(), 6, 127);
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
<label>Claude OAuth Token <small>(终端运行 claude setup-token 获取, sk-ant-oat01-...)</small></label>
<textarea name="ctok"></textarea>
<label>Codex Access Token <small>(~/.codex/auth.json 的 tokens.access_token)</small></label>
<textarea name="cxat"></textarea>
<label>Codex Refresh Token <small>(tokens.refresh_token, 用于自动续期)</small></label>
<textarea name="cxrt"></textarea>
<button type="submit">保存并重启</button>
</form>
<p><small>提示: 可用仓库里的 tools/get_tokens.py 一键打印以上三个值。</small></p>
</body></html>)html";

static void handleSave() {
  prefs.begin("quotatv", false);
  prefs.putString("ssid",  server.arg("ssid"));
  prefs.putString("pass",  server.arg("pass"));
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
  String apName = AP_SSID_PREFIX + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
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

// STA 模式下的辅助端点
static void startStaServer() {
  server.on("/", []() {
    String s = "QuotaTV\nclaude 5h=" + String(claudeData.session.usedPct) +
               " 7d=" + String(claudeData.weekly.usedPct) +
               "\ncodex  5h=" + String(codexData.session.usedPct) +
               " 7d=" + String(codexData.weekly.usedPct) + "\n";
    server.send(200, "text/plain", s);
  });
  server.on("/status", []() {
    JsonDocument doc;
    doc["claude"]["session"] = claudeData.session.usedPct;
    doc["claude"]["weekly"]  = claudeData.weekly.usedPct;
    doc["claude"]["err"]     = claudeData.lastErr;
    doc["codex"]["session"]  = codexData.session.usedPct;
    doc["codex"]["weekly"]   = codexData.weekly.usedPct;
    doc["codex"]["err"]      = codexData.lastErr;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/reset", HTTP_POST, []() {
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
  cfg.claudeToken  = prefs.getString("c_tok", "");
  cfg.codexAccess  = prefs.getString("cx_at", "");
  cfg.codexRefresh = prefs.getString("cx_rt", "");
  prefs.end();
}

void setup() {
  buzzInit();                                  // 最先: 低电平触发, 避免上电长鸣
  Serial.begin(115200);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  pinMode(PIN_TFT_BL, OUTPUT);
  analogWrite(PIN_TFT_BL, 200);                // 背光 ~80%

  tft.init();
  tft.setRotation(3);                          // 横屏 160x128; 方向反了改成 1
  canvas.setColorDepth(16);
  canvas.createSprite(160, 128);

  loadCfg();

  // 开机按住 BOOT 键 1 秒 → 强制配置模式
  delay(80);
  bool forcePortal = digitalRead(PIN_BOOT_BTN) == LOW;
  if (forcePortal) { delay(1000); forcePortal = digitalRead(PIN_BOOT_BTN) == LOW; }

  if (cfg.ssid.isEmpty() || forcePortal) { startPortal(); return; }

  drawMessage("Connecting WiFi", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    drawMessage("WiFi failed", "hold BOOT & reboot", "to re-configure");
    delay(15000);
    ESP.restart();
  }

  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);
  drawMessage("Syncing time...");
  struct tm t;
  getLocalTime(&t, 8000);

  startStaServer();
  beep(1, 60);                                 // 就绪提示
  drawScreen();
}

void loop() {
  if (portalMode) { dns.processNextRequest(); server.handleClient(); return; }

  server.handleClient();

  uint32_t nowMs = millis();
  if (!firstPollDone || nowMs - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = nowMs;
    fetchClaude();
    fetchCodex();
    firstPollDone = true;
    drawScreen();
  }

  // 每秒刷新一次 (时钟/倒计时)
  static uint32_t lastDraw = 0;
  if (nowMs - lastDraw >= 1000) { lastDraw = nowMs; drawScreen(); }

  // 运行中按 BOOT 键: 立即强制刷新
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    delay(30);
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
      lastPollMs = 0; firstPollDone = false;
    }
  }
  delay(10);
}
