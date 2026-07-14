#pragma once

// ================= 引脚 (ESP32-C3 DevKitM-1) =================
// TFT SPI 引脚在 platformio.ini 的 build_flags 里 (SCLK=4 MOSI=6 CS=7 DC=5 RST=10)
#define PIN_TFT_BL     3    // 背光, PWM
#define PIN_BUZZER     1    // 有源蜂鸣器, 低电平触发
#define PIN_BOOT_BTN   9    // 板载 BOOT 键: 开机长按进入配置模式

// ================= 轮询 =================
#define POLL_INTERVAL_MS      (120UL * 1000UL)  // 每 2 分钟刷新一次额度
#define HTTP_TIMEOUT_MS       15000
#define STALE_AFTER_MS        (15UL * 60UL * 1000UL) // 超过 15 分钟没有成功数据视为过期

// ================= Claude =================
// 通过发送 max_tokens=1 的最小请求, 从响应头读取额度利用率:
//   anthropic-ratelimit-unified-5h-utilization  (0.0~1.0)
//   anthropic-ratelimit-unified-7d-utilization  (0.0~1.0)
//   anthropic-ratelimit-unified-5h-reset / -7d-reset (unix epoch)
#define CLAUDE_API_HOST   "api.anthropic.com"
#define CLAUDE_API_PATH   "/v1/messages"
#define CLAUDE_PROBE_MODEL "claude-haiku-4-5-20251001"

// ================= Codex (ChatGPT) =================
#define CODEX_USAGE_URL   "https://chatgpt.com/backend-api/wham/usage"
#define OPENAI_TOKEN_URL  "https://auth.openai.com/oauth/token"
// Codex CLI 的公开 OAuth client id (与 ~/.codex/auth.json 配套)
#define CODEX_CLIENT_ID   "app_EMoamEEZ73f0CkXaXp7hrann"
// access token 剩余寿命低于该值时尝试用 refresh token 刷新 (秒)
#define CODEX_REFRESH_MARGIN_S  (24UL * 3600UL)

// ================= 蜂鸣 =================
// 额度重置判定: 利用率相比上次下降超过该百分点, 或 reset 时间戳被跨过
#define RESET_DROP_THRESHOLD  25.0f
#define QUIET_HOUR_START  23   // 静音时段 (本地时间)
#define QUIET_HOUR_END    8

// ================= 时间 =================
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.windows.com"
#define TZ_STRING     "CST-8"   // 中国标准时间

// ================= 配网 AP =================
#define AP_SSID_PREFIX  "QuotaTV-"
#define AP_IP           IPAddress(192, 168, 4, 1)
