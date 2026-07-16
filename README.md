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

### Agent 获取与运行

**推荐**:直接从 [Releases](https://github.com/mahonzhan/quota-tv/releases) 下载对应平台的 quota-agent(打 tag 后由 GitHub Actions 自动构建:Windows amd64 / macOS arm64+amd64 / Linux amd64)。

双击(或命令行)运行后**驻留系统托盘**(Windows 任务栏 / macOS 菜单栏 / Linux 状态区),托盘菜单提供:最近推送状态、立即推送、**开机自启开关**(写入各平台原生自启机制:注册表 Run / LaunchAgent / autostart desktop)、退出。

托盘直接显示额度:**macOS** 菜单栏图标旁显示文字 `CL 62/41 · CX 30/71`(CL=Claude, CX=Codex, 数值为 5h/7d 已用%);**Windows/Linux** 托盘不支持文字,数字直接渲染进图标——上行橙色为 Claude 5h、下行白色为 Codex 5h,悬停 tooltip 看完整数据。

macOS 凭据说明:Claude Code 在 mac 上把凭据存**钥匙串**(非文件),Agent 会自动调 `security` 读取(服务名 `Claude Code-credentials`),**首次运行会弹钥匙串授权框,点"始终允许"**即可;也可用 `claude setup-token` + 环境变量 `CLAUDE_CODE_OAUTH_TOKEN` 绕过钥匙串。

**macOS 下载 `.app.zip`**(而非裸二进制):解压后双击即驻留菜单栏,不开终端、不占 Dock。首次打开因未签名会被 Gatekeeper 拦截,右键 → 打开 → 再点打开即可。裸二进制版供命令行/launchd 场景使用。

命令行参数(托盘模式同样生效):

```bash
quota-agent                  # 托盘常驻, 自动发现设备, 每 2 分钟推送
quota-agent -once            # 调试: 发现设备并推一次后退出
quota-agent -nogui           # 无托盘纯命令行 (服务器/systemd 场景)
quota-agent -name quotatv-a1b2   # 多设备时指定
quota-agent -all             # 多设备全部推送
quota-agent -device http://192.168.1.50   # 广播不通时手动指定
```

自己编译(`cd tools/quota-agent`):

```bash
# Windows — 必须加 -H windowsgui, 否则 exe 启动会弹黑色控制台窗口
go build -ldflags "-s -w -H windowsgui" -o quota-agent.exe

# macOS / Linux
go build -ldflags "-s -w" -o quota-agent
```

注意:`-H windowsgui` 的 GUI 版没有控制台,`log` 输出不可见;Windows 上调试(看发现/推送日志)时用不带该参数的构建跑 `quota-agent -once`。macOS 版依赖 cgo,需在 mac 上编译(Releases 里的由 CI 的 macos runner 构建)。

exe 文件图标:仓库已提交 `rsrc_windows_amd64.syso`(由 `icon.ico` 经 [akavel/rsrc](https://github.com/akavel/rsrc) 生成),`go build` 自动链接,无需额外操作;改图标时重新生成该文件即可(`go run github.com/akavel/rsrc@latest -ico icon.ico -o rsrc_windows_amd64.syso`)。macOS 裸二进制没有文件图标概念,要图标需打包成 .app bundle(未做,菜单栏图标不受影响)。

### 发布流程 (维护者)

```bash
git tag v1.0.0 && git push origin v1.0.0
```

`.github/workflows/release.yml` 会在三平台 runner 上构建 4 个二进制并自动创建 GitHub Release。

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
