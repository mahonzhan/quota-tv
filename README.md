# QuotaTV — Claude + ChatGPT 订阅额度桌面显示器

ESP32-C3 DevKitM-1 + 1.8" ST7735 的复古小电脑,单屏显示 Claude Code 与 ChatGPT(Codex) 的 5h/周额度使用率和重置倒计时,额度重置时蜂鸣提醒。设备直连两家接口,**不需要电脑常驻脚本**。

```
┌──────────────────────────────┐
│ QUOTA TV          14:32  ●   │
│ CLAUDE        5h reset 2h14m │
│  5H ██████████░░░░░░░   62%  │
│  7D ██████░░░░░░░░░░░   41%  │
│ ──────────────────────────── │
│ CODEX         5h reset 4h02m │
│  5H █████░░░░░░░░░░░░   30%  │
│  7D ████████████░░░░░   71%  │
│ 192.168.1.23                 │
└──────────────────────────────┘
```

## 1. 硬件与接线

| ST7735 屏 (8针) | ESP32-C3 DevKitM-1 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL / SCK | GPIO4 |
| SDA / MOSI | GPIO6 |
| RES / RST | GPIO10 |
| DC / A0 | GPIO5 |
| CS | GPIO7 |
| BLK / LED | GPIO3 |

| 蜂鸣器模块 (低电平触发) | DevKitM-1 |
|---|---|
| VCC | 3V3 (响声小可接 5V/VUSB) |
| GND | GND |
| I/O | GPIO1 |

> 避开了 C3 的 strapping 引脚 (GPIO2/8/9)。GPIO9 是板载 BOOT 键,固件里用作按钮。

## 2. 刷固件

```bash
pip install platformio
cd firmware
pio run -t upload        # USB 连接 DevKitM-1
pio device monitor       # 看日志 (115200)
```

### ST7735 变体调试(几乎必经步骤)

1.8" ST7735 有红标/绿标/黑标多种批次。若画面偏移、镜像或颜色不对,改 `platformio.ini`:

- 偏移/花屏 → 把 `ST7735_GREENTAB` 依次换成 `ST7735_REDTAB` / `ST7735_BLACKTAB` / `ST7735_GREENTAB2` / `ST7735_GREENTAB3`
- 蓝橙互换 → `TFT_RGB_ORDER` 在 `TFT_BGR` / `TFT_RGB` 之间切换
- 方向反了 → `main.cpp` 里 `tft.setRotation(3)` 改成 `1`

## 3. 获取凭据

在装有 Claude Code 和 Codex CLI 的电脑上:

```bash
# Claude: 生成长期 OAuth token (推荐)
claude setup-token        # 输出 sk-ant-oat01-...

# Codex: 一键打印 access/refresh token
python tools/get_tokens.py
```

## 4. 配置设备

1. 首次上电(或开机按住 BOOT 键 1 秒)进入配置模式,屏幕显示热点名
2. 手机/电脑连接 `QuotaTV-xxxx` 热点,打开 `http://192.168.4.1`
3. 填 WiFi(2.4GHz)、Claude token、Codex access + refresh token,保存重启

运行后设备每 2 分钟刷新一次;按一下 BOOT 键立即刷新。浏览器访问 `http://<设备IP>/status` 可看 JSON 数据;`POST /reset` 清空配置。

## 5. 数据来源(与风险)

- **Claude**:发送 `max_tokens=1` 的最小请求,从响应头 `anthropic-ratelimit-unified-5h/7d-utilization`(0~1)和 `-reset`(epoch)读取。每次探测消耗几个 token,量级可忽略。
- **Codex**:`GET chatgpt.com/backend-api/wham/usage`,零消耗;access token 过期时自动用 refresh token 到 `auth.openai.com` 续期并存入 NVS。
- 两个都是**非官方接口**,可能随时变化。解析已尽量写得宽容,失败时屏幕显示错误码,串口有原始响应片段可排查。
- 若设备端 refresh 与电脑上 Codex CLI 的凭据互相顶掉(表现为某一边频繁要求重新登录),改为定期从电脑重新粘贴 access token、refresh token 留空。
- 固件对 TLS 用 `setInsecure()`(不校验证书链)。家庭内网风险可接受;介意的话把各域名根证书写进固件。

## 6. 蜂鸣逻辑

- 任一窗口利用率相比上次骤降 ≥25 个百分点,或跨过 reset 时间点 → 蜂鸣(Claude 两声,Codex 三声)
- 静音时段 23:00–08:00(`config.h` 可改)
- 开机就绪一声短鸣

## 7. 外壳打印 (case/) — Claude bot 橙色机器人造型

用**橙色 PLA**打印(Claude 品牌色约 #D97757),效果图见 `case/assembly_preview.png`。

- `body.stl` — 机器人头主壳(74×64×30, 大圆角):前脸朝下打印,免支撑;顶部有天线插孔
- `antenna.stl` — 球头天线:竖直打印,杆 φ5.6 插入 φ6 孔,过松点胶
- `back.stl` — 后盖:平放,含 micro-USB 槽、蜂鸣孔、散热缝
- `stand.stl` — 12° 仰角底座,带两只小脚趾
- 参数化源文件 `quota_tv_case.scad` / `gen_case.py`:**打印前先量你的屏幕模块 PCB 实际尺寸**,改 `scr_pcb_w/h` 后重新导出更稳(各家 1.8" 模块 PCB 尺寸不一)
- 装配:屏幕贴入前脸内侧定位框(双面胶/热熔胶)→ DevKitM-1 平放壳底、USB 口对准后盖槽 → 蜂鸣器贴后盖出音孔附近 → 杜邦线连好后插上后盖 → 插天线 → 卡入底座

## 8. 安全提醒

token 等同账号使用权,只存在设备 NVS 里,不要把配置好的设备借人;闲鱼出手前 `POST /reset` 或重刷固件。
