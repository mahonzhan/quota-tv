package main

// 开机自启: 三平台原生机制, 无第三方依赖
//   Windows: HKCU\...\Run 注册表项 (reg.exe)
//   macOS:   ~/Library/LaunchAgents/com.quotatv.agent.plist
//   Linux:   ~/.config/autostart/quota-agent.desktop

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

const winRunKey = `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
const appName = "QuotaTVAgent"

func exePath() string {
	p, err := os.Executable()
	if err != nil {
		return "quota-agent"
	}
	return p
}

func macPlistPath() string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "Library", "LaunchAgents", "com.quotatv.agent.plist")
}

func linuxDesktopPath() string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".config", "autostart", "quota-agent.desktop")
}

func autostartEnabled() bool {
	switch runtime.GOOS {
	case "windows":
		return exec.Command("reg", "query", winRunKey, "/v", appName).Run() == nil
	case "darwin":
		_, err := os.Stat(macPlistPath())
		return err == nil
	default:
		_, err := os.Stat(linuxDesktopPath())
		return err == nil
	}
}

func enableAutostart() error {
	exe := exePath()
	switch runtime.GOOS {
	case "windows":
		return exec.Command("reg", "add", winRunKey, "/v", appName,
			"/t", "REG_SZ", "/d", `"`+exe+`"`, "/f").Run()
	case "darwin":
		plist := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.quotatv.agent</string>
  <key>ProgramArguments</key><array><string>%s</string></array>
  <key>RunAtLoad</key><true/>
</dict></plist>
`, exe)
		p := macPlistPath()
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		return os.WriteFile(p, []byte(plist), 0o644)
	default:
		desktop := fmt.Sprintf(`[Desktop Entry]
Type=Application
Name=QuotaTV Agent
Exec=%s
X-GNOME-Autostart-enabled=true
`, exe)
		p := linuxDesktopPath()
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		return os.WriteFile(p, []byte(desktop), 0o644)
	}
}

func disableAutostart() error {
	switch runtime.GOOS {
	case "windows":
		return exec.Command("reg", "delete", winRunKey, "/v", appName, "/f").Run()
	case "darwin":
		return os.Remove(macPlistPath())
	default:
		return os.Remove(linuxDesktopPath())
	}
}
