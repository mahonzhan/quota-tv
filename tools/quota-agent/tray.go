package main

// 系统托盘常驻 (Windows 任务栏 / macOS 菜单栏 / Linux StatusNotifier)
// 基于 fyne.io/systray: Windows 纯 syscall, Linux 纯 Go (D-Bus), macOS cgo

import (
	_ "embed"
	"fmt"
	"runtime"
	"time"

	"fyne.io/systray"
)

//go:embed icon.png
var iconPNG []byte

//go:embed icon.ico
var iconICO []byte

func trayIcon() []byte {
	if runtime.GOOS == "windows" {
		return iconICO
	}
	return iconPNG
}

func runTray(targets []string, interval time.Duration) {
	systray.Run(func() { trayReady(targets, interval) }, nil)
}

func trayReady(targets []string, interval time.Duration) {
	systray.SetIcon(trayIcon())
	systray.SetTooltip("QuotaTV Agent")

	mStatus := systray.AddMenuItem("等待首次推送...", "最近一次推送结果")
	mStatus.Disable()
	systray.AddSeparator()
	mPush := systray.AddMenuItem("立即推送", "马上拉取额度并推送到设备")
	mAuto := systray.AddMenuItemCheckbox("开机自启", "登录时自动运行", autostartEnabled())
	systray.AddSeparator()
	mQuit := systray.AddMenuItem("退出", "")

	update := func() {
		s, frame := cycle(targets)
		mStatus.SetTitle(s)
		systray.SetTooltip("QuotaTV Agent — " + s)

		var cs, cw, gs, gw *float64
		if frame.Claude != nil {
			cs, cw = frame.Claude.Session, frame.Claude.Weekly
		}
		if frame.Codex != nil {
			gs, gw = frame.Codex.Session, frame.Codex.Weekly
		}
		if runtime.GOOS == "darwin" {
			// macOS: 菜单栏原生文字标题, 显示 5h/7d 已用%
			systray.SetTitle(fmt.Sprintf("C%s/%s G%s/%s",
				pctStr(cs), pctStr(cw), pctStr(gs), pctStr(gw)))
		} else {
			// Windows/Linux: 数字画进图标 (上橙 Claude 5h, 下白 Codex 5h)
			p := renderNumberPNG(pctStr(cs), pctStr(gs))
			if runtime.GOOS == "windows" {
				systray.SetIcon(pngToICO(p))
			} else {
				systray.SetIcon(p)
			}
		}
	}

	go func() {
		update()
		tick := time.NewTicker(interval)
		defer tick.Stop()
		for {
			select {
			case <-tick.C:
				update()
			case <-mPush.ClickedCh:
				update()
			case <-mAuto.ClickedCh:
				if mAuto.Checked() {
					if disableAutostart() == nil {
						mAuto.Uncheck()
					}
				} else {
					if enableAutostart() == nil {
						mAuto.Check()
					}
				}
			case <-mQuit.ClickedCh:
				systray.Quit()
				return
			}
		}
	}()
}
