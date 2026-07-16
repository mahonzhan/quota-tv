// quota-agent — QuotaTV 主机 Agent (Windows/macOS/Linux)
//
// 读取本机 Claude Code / Codex CLI 凭据, 拉取两家订阅额度,
// 推送到 QuotaTV 设备 (POST /push)。token 不离开本机。
//
// 用法:
//
//	quota-agent                              # 默认推 http://quotatv.local, 每 120s
//	quota-agent -device http://192.168.1.50  # 手动指定设备
//	quota-agent -once                        # 只跑一次 (调试)
//
// 交叉编译:
//
//	GOOS=windows GOARCH=amd64 go build -ldflags "-s -w" -o quota-agent.exe
//	GOOS=darwin  GOARCH=arm64 go build -ldflags "-s -w" -o quota-agent-mac
//	GOOS=linux   GOARCH=amd64 go build -ldflags "-s -w" -o quota-agent-linux
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const discoveryPort = 18324

const (
	anthropicURL = "https://api.anthropic.com/v1/messages"
	probeModel   = "claude-haiku-4-5-20251001"
	codexUsage   = "https://chatgpt.com/backend-api/wham/usage"
)

var httpc = &http.Client{Timeout: 20 * time.Second}

// ---------- 推送帧 ----------

type providerPush struct {
	Session      *float64 `json:"session,omitempty"` // 已用 %, 0~100
	Weekly       *float64 `json:"weekly,omitempty"`
	SessionReset *int64   `json:"sessionReset,omitempty"` // unix epoch 秒
	WeeklyReset  *int64   `json:"weeklyReset,omitempty"`
}

type pushFrame struct {
	Claude   *providerPush `json:"claude,omitempty"`
	Codex    *providerPush `json:"codex,omitempty"`
	Interval int64         `json:"interval,omitempty"` // 推送间隔(秒), 设备据此算 stale 阈值
}

var pushIntervalSecs int64 // main 里根据 -interval 赋值

// ---------- Claude: 最小探测请求, 读响应头 ----------

func parseClaudeCred(raw []byte) (string, error) {
	var cred struct {
		ClaudeAiOauth struct {
			AccessToken string `json:"accessToken"`
		} `json:"claudeAiOauth"`
	}
	if err := json.Unmarshal(raw, &cred); err != nil || cred.ClaudeAiOauth.AccessToken == "" {
		return "", fmt.Errorf("解析 Claude 凭据 JSON 失败")
	}
	return cred.ClaudeAiOauth.AccessToken, nil
}

// 查找顺序: 环境变量 > ~/.claude/.credentials.json (Linux/Windows)
//
//	> macOS 钥匙串 (Claude Code 在 mac 上存钥匙串, 无文件)
func claudeToken() (string, error) {
	if t := os.Getenv("CLAUDE_CODE_OAUTH_TOKEN"); t != "" {
		return t, nil
	}
	home, _ := os.UserHomeDir()
	dir := os.Getenv("CLAUDE_CONFIG_DIR")
	if dir == "" {
		dir = filepath.Join(home, ".claude")
	}
	if raw, err := os.ReadFile(filepath.Join(dir, ".credentials.json")); err == nil {
		return parseClaudeCred(raw)
	}
	if runtime.GOOS == "darwin" {
		// 首次访问会弹钥匙串授权框, 选"始终允许"
		out, err := exec.Command("security", "find-generic-password",
			"-s", "Claude Code-credentials", "-w").Output()
		if err == nil && len(bytes.TrimSpace(out)) > 0 {
			return parseClaudeCred(bytes.TrimSpace(out))
		}
	}
	return "", fmt.Errorf("未找到 Claude 凭据; 可运行 `claude setup-token` 并设置环境变量 CLAUDE_CODE_OAUTH_TOKEN")
}

func fetchClaude() (*providerPush, error) {
	tok, err := claudeToken()
	if err != nil {
		return nil, err
	}
	body := fmt.Sprintf(`{"model":%q,"max_tokens":1,"messages":[{"role":"user","content":"hi"}]}`, probeModel)
	req, _ := http.NewRequest("POST", anthropicURL, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+tok)
	req.Header.Set("anthropic-version", "2023-06-01")
	req.Header.Set("anthropic-beta", "oauth-2025-04-20")
	resp, err := httpc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)

	p := &providerPush{}
	if f, ok := headerFloat(resp, "anthropic-ratelimit-unified-5h-utilization"); ok {
		f *= 100
		p.Session = &f
	}
	if f, ok := headerFloat(resp, "anthropic-ratelimit-unified-7d-utilization"); ok {
		f *= 100
		p.Weekly = &f
	}
	if n, ok := headerInt(resp, "anthropic-ratelimit-unified-5h-reset"); ok {
		p.SessionReset = &n
	}
	if n, ok := headerInt(resp, "anthropic-ratelimit-unified-7d-reset"); ok {
		p.WeeklyReset = &n
	}
	if p.Session == nil && p.Weekly == nil {
		return nil, fmt.Errorf("响应无额度头 (HTTP %d), token 可能失效", resp.StatusCode)
	}
	return p, nil
}

func headerFloat(r *http.Response, k string) (float64, bool) {
	v := r.Header.Get(k)
	if v == "" {
		return 0, false
	}
	f, err := strconv.ParseFloat(v, 64)
	return f, err == nil
}

func headerInt(r *http.Response, k string) (int64, bool) {
	v := r.Header.Get(k)
	if v == "" {
		return 0, false
	}
	n, err := strconv.ParseInt(v, 10, 64)
	return n, err == nil
}

// ---------- Codex: wham/usage ----------

func codexToken() (string, error) {
	home, _ := os.UserHomeDir()
	dir := os.Getenv("CODEX_HOME")
	if dir == "" {
		dir = filepath.Join(home, ".codex")
	}
	raw, err := os.ReadFile(filepath.Join(dir, "auth.json"))
	if err != nil {
		return "", fmt.Errorf("未找到 Codex 凭据 (%v); 请先运行 codex 并登录", err)
	}
	var auth struct {
		Tokens struct {
			AccessToken string `json:"access_token"`
		} `json:"tokens"`
	}
	if err := json.Unmarshal(raw, &auth); err != nil || auth.Tokens.AccessToken == "" {
		return "", fmt.Errorf("解析 auth.json 失败")
	}
	return auth.Tokens.AccessToken, nil
}

// 宽容解析: 与固件 parseCodexWindow 同等的字段名/类型兼容性
type codexWindow map[string]any

func (w codexWindow) num(keys ...string) (float64, bool) {
	for _, k := range keys {
		if v, ok := w[k]; ok {
			if f, ok2 := v.(float64); ok2 { // encoding/json 所有数字都是 float64
				return f, true
			}
		}
	}
	return 0, false
}

func (w codexWindow) fill(pct **float64, reset **int64) {
	if w == nil {
		return
	}
	if f, ok := w.num("used_percent", "usage_percent", "used"); ok {
		v := f
		*pct = &v
	}
	if f, ok := w.num("resets_at", "reset_at"); ok { // 绝对时间戳
		t := int64(f)
		if t > 4102444800 { // 毫秒时间戳兜底
			t /= 1000
		}
		*reset = &t
	} else if f, ok := w.num("resets_in_seconds", "reset_after_seconds"); ok { // 相对秒数
		t := time.Now().Unix() + int64(f)
		*reset = &t
	}
}

func fetchCodex() (*providerPush, error) {
	tok, err := codexToken()
	if err != nil {
		return nil, err
	}
	req, _ := http.NewRequest("GET", codexUsage, nil)
	req.Header.Set("Authorization", "Bearer "+tok)
	req.Header.Set("Accept", "application/json")
	resp, err := httpc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == 401 {
		// Codex CLI 会自己刷新 auth.json; 提示用户跑一次 codex 即可
		return nil, fmt.Errorf("401: token 过期, 运行一次 `codex` 让 CLI 刷新 auth.json")
	}
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	var doc map[string]any
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("JSON 解析失败: %v", err)
	}
	rl, _ := doc["rate_limit"].(map[string]any)
	if rl == nil {
		rl, _ = doc["rate_limits"].(map[string]any)
	}
	var primary, secondary codexWindow
	if rl != nil {
		if m, ok := rl["primary_window"].(map[string]any); ok {
			primary = codexWindow(m)
		}
		if m, ok := rl["secondary_window"].(map[string]any); ok {
			secondary = codexWindow(m)
		}
	}
	p := &providerPush{}
	primary.fill(&p.Session, &p.SessionReset)
	secondary.fill(&p.Weekly, &p.WeeklyReset)
	if p.Session == nil && p.Weekly == nil {
		return nil, fmt.Errorf("接口结构变化, 原始响应: %.200s", string(raw))
	}
	if p.SessionReset == nil && p.WeeklyReset == nil {
		// 有百分比但没识别出 reset 字段: 打印窗口原始内容便于排查
		log.Printf("[codex ] 未识别 reset 字段, primary_window 原始: %v", primary)
	}
	return p, nil
}

// ---------- 推送 ----------

func push(device string, frame pushFrame) error {
	raw, _ := json.Marshal(frame)
	resp, err := httpc.Post(strings.TrimRight(device, "/")+"/push",
		"application/json", bytes.NewReader(raw))
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)
	if resp.StatusCode != 200 {
		return fmt.Errorf("设备返回 HTTP %d", resp.StatusCode)
	}
	return nil
}

// 拉取一次数据, 推送到所有目标 (多设备只探测一次, 不重复消耗)
// 返回状态摘要 (托盘菜单显示) 和本次数据帧 (托盘图标/标题渲染)
func cycle(targets []string) (string, pushFrame) {
	frame := pushFrame{Interval: pushIntervalSecs}
	var status string
	if p, err := fetchClaude(); err != nil {
		log.Printf("[claude] %v", err)
		status = "Claude ERR"
	} else {
		frame.Claude = p
		log.Printf("[claude] 5h=%.1f%% 7d=%.1f%%", deref(p.Session), deref(p.Weekly))
		status = fmt.Sprintf("Claude %.0f/%.0f%%", deref(p.Session), deref(p.Weekly))
	}
	if p, err := fetchCodex(); err != nil {
		log.Printf("[codex ] %v", err)
		status += " · Codex ERR"
	} else {
		frame.Codex = p
		log.Printf("[codex ] 5h=%.1f%% 7d=%.1f%%", deref(p.Session), deref(p.Weekly))
		status += fmt.Sprintf(" · Codex %.0f/%.0f%%", deref(p.Session), deref(p.Weekly))
	}
	if frame.Claude == nil && frame.Codex == nil {
		log.Printf("[push  ] 无数据, 跳过")
		return status + " (未推送)", frame
	}
	pushOK := true
	for _, t := range targets {
		if err := push(t, frame); err != nil {
			log.Printf("[push  ] %s: %v", t, err)
			pushOK = false
		} else {
			log.Printf("[push  ] ok -> %s", t)
		}
	}
	if !pushOK {
		status += " (推送失败)"
	}
	return status, frame
}

func deref(f *float64) float64 {
	if f == nil {
		return -1
	}
	return *f
}

// ---------- UDP 广播发现 (不依赖 mDNS) ----------

type deviceInfo struct {
	Name string `json:"name"`
	IP   string `json:"ip"`
	Mode string `json:"mode"`
}

func discover(timeout time.Duration) []deviceInfo {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		log.Printf("[发现] UDP 监听失败: %v", err)
		return nil
	}
	defer conn.Close()
	dst := &net.UDPAddr{IP: net.IPv4bcast, Port: discoveryPort}
	for i := 0; i < 3; i++ { // 广播 3 次防丢包
		conn.WriteToUDP([]byte("QUOTATV_DISCOVER?"), dst)
		time.Sleep(80 * time.Millisecond)
	}
	conn.SetReadDeadline(time.Now().Add(timeout))
	seen := map[string]deviceInfo{}
	buf := make([]byte, 512)
	for {
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			break // 超时
		}
		var d deviceInfo
		if json.Unmarshal(buf[:n], &d) == nil && d.IP != "" {
			seen[d.IP] = d
		}
	}
	out := make([]deviceInfo, 0, len(seen))
	for _, d := range seen {
		out = append(out, d)
	}
	return out
}

// 解析推送目标: -device 显式指定 > UDP 发现 > mDNS 兜底
func resolveTargets(device, name string, all bool) []string {
	if device != "" {
		return []string{device}
	}
	found := discover(2 * time.Second)
	if len(found) == 0 {
		log.Printf("[发现] UDP 无响应, 兜底尝试 mDNS http://quotatv.local (可用 -device 手动指定 IP)")
		return []string{"http://quotatv.local"}
	}
	var targets []string
	for _, d := range found {
		log.Printf("[发现] %s @ %s (mode=%s)", d.Name, d.IP, d.Mode)
		if name != "" && d.Name != name {
			continue
		}
		targets = append(targets, "http://"+d.IP)
	}
	if name == "" && len(targets) > 1 && !all {
		log.Fatalf("发现 %d 台设备, 请用 -name quotatv-xxxx 指定一台, 或 -all 全部推送", len(targets))
	}
	if len(targets) == 0 {
		log.Fatalf("未找到名为 %q 的设备", name)
	}
	return targets
}

func main() {
	device := flag.String("device", "", "设备地址 (留空则自动发现, 如 http://192.168.1.50)")
	name := flag.String("name", "", "多设备时指定设备名 (如 quotatv-a1b2)")
	all := flag.Bool("all", false, "推送到发现的所有设备")
	interval := flag.Duration("interval", 120*time.Second, "轮询间隔")
	once := flag.Bool("once", false, "只执行一次后退出")
	nogui := flag.Bool("nogui", false, "不驻留托盘, 纯命令行运行 (服务器/systemd 用)")
	flag.Parse()

	pushIntervalSecs = int64(interval.Seconds())
	targets := resolveTargets(*device, *name, *all)
	log.Printf("quota-agent 启动: targets=%v interval=%s", targets, *interval)

	if *once {
		cycle(targets)
		return
	}
	if *nogui {
		cycle(targets)
		for range time.Tick(*interval) {
			cycle(targets)
		}
		return
	}
	runTray(targets, *interval) // 默认: 托盘常驻
}
