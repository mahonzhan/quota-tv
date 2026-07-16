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

1.8" ST7735 有红标/绿标/黑标多种批次。本项目实测的蓝板 (丝印 1.8'TFT128*RGB*160) 为 **REDTAB + TFT_RGB**,已设为默认。其他批次若画面异常,改 `platformio.ini`:

- 偏移/花屏 → 把 `ST7735_REDTAB` 依次换成 `ST7735_BLACKTAB` / `ST7735_GREENTAB` / `ST7735_GREENTAB2` / `ST7735_GREENTAB3`
- 蓝橙互换 → `TFT_RGB_ORDER` 在 `TFT_RGB` / `TFT_BGR` 之间切换
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

1. 首次上电自动进入配置模式;已配置的设备**运行中长按 BOOT 3 秒**(听到一声蜂鸣后松手)或 `curl -X POST http://<设备IP>/portal` 重新进入。注意**不要按住 BOOT 上电**——C3 会进串口下载模式导致黑屏(拔电重插即恢复)
2. 手机/电脑连接 `QuotaTV-xxxx` 热点,打开 `http://192.168.4.1`
3. 填 WiFi(2.4GHz)、Claude token、Codex access + refresh token,保存重启

运行后设备每 2 分钟刷新一次;按一下 BOOT 键立即刷新。浏览器访问 `http://<设备IP>/status` 可看 JSON 数据;`POST /reset` 清空配置。

## 5. 双工作模式

配置页可选两种模式:

**直连模式(默认)**:设备自己调两家接口,不依赖电脑。需在配置页填入 token。

**主机 Agent 模式**:token 不上设备,由电脑上的 `quota-agent`(Go 编写,单二进制零依赖)读取本机 Claude Code / Codex CLI 凭据,拉取额度后经局域网 HTTP 推送到设备(`POST /push`)。Codex token 由 CLI 自动续期,Agent 直接受益。代价是电脑关机后设备显示 stale。

### 设备发现(Agent 模式)

三层机制,按优先级:

1. **UDP 广播发现(主路径,不依赖 mDNS)**:Agent 向 18324 端口广播 `QUOTATV_DISCOVER?`,设备回 `{name, ip, mode}`。同网多台设备各有唯一名 `quotatv-xxxx`(MAC 后缀,屏幕底行与 IP 交替显示):发现多台时用 `-name quotatv-a1b2` 指定,或 `-all` 全部推送(数据只拉一次)
2. mDNS 兜底:每台设备注册 `quotatv-xxxx.local`
3. 手动指定:`-device http://192.168.x.x`(AP 隔离等广播不通的网络用这个)

### Agent 编译与运行

```bash
cd tools/quota-agent
go build -ldflags "-s -w" -o quota-agent        # 本平台
GOOS=windows GOARCH=amd64 go build -o quota-agent.exe   # 交叉编译其他平台
GOOS=darwin  GOARCH=arm64 go build -o quota-agent-mac
GOOS=linux   GOARCH=amd64 go build -o quota-agent-linux

./quota-agent -once      # 调试: 发现设备并推一次
./quota-agent            # 常驻: 每 2 分钟推一次
```

开机自启:

- **Windows**: `schtasks /create /tn QuotaAgent /sc onlogon /tr "C:\path\quota-agent.exe"`
- **macOS**: `launchd` 用户 LaunchAgent(`~/Library/LaunchAgents/`,RunAtLoad + KeepAlive)
- **Linux**: systemd user unit(`~/.config/systemd/user/quota-agent.service`,`ExecStart=... Restart=always`,`systemctl --user enable --now quota-agent`)

## 6. 数据来源(与风险)

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

适配硬件:1.8" TFT128×RGB×160 蓝板屏 + **合宙 Core-ESP32C3** + 三针立式有源蜂鸣器模块,飞线直焊。

- `body.stl` — 机器人头主壳(74×64×30, 大圆角):前脸朝下打印,免支撑;顶部天线插孔,右侧壁后缘 USB-C 出线缺口
- `antenna.stl` — 球头天线:竖直打印,杆 φ5.6 插入 φ6 孔,过松点胶
- `back.stl` — 后盖:平放打印;含 **BOOT/RST 两个按钮孔**、蜂鸣孔、散热缝、唇部 USB 缺口
- `button_plunger_x2.stl` — 按钮顶杆,打印 2 个:蘑菇头朝内放入孔中,再贴板,即可从壳外按 BOOT/RST
- `stand.stl` — 12° 仰角底座,带两只小脚趾
- 装配:屏幕贴入前脸内侧定位框 → 顶杆入孔 → **合宙板元件面朝后盖**、USB-C 对准 -x 缺口方向,按钮对准顶杆后用双面胶/热熔胶贴在后盖内侧 → 蜂鸣器贴出音孔附近 → 飞线焊好插上后盖 → 插天线 → 卡入底座
- 源文件 `gen_case.py`(STL 以它为准):打印前量三个尺寸并改参数重新生成——屏模块 PCB 宽高(`SCR_PCB_W/H`)、合宙板 USB 端边缘到按钮中心距(`BTN_FROM_USB`)、两按钮中心距(`BTN_GAP`)

## 8. 安全提醒

token 等同账号使用权,只存在设备 NVS 里,不要把配置好的设备借人;闲鱼出手前 `POST /reset` 或重刷固件。
