#!/usr/bin/env python3
"""
get_tokens.py — 打印 QuotaTV 配置页需要的三个凭据 (Windows/macOS/Linux)

  1. Claude OAuth Token   : 优先读 ~/.claude/.credentials.json;
                            读不到时提示用 `claude setup-token` 生成 (推荐, 长期有效)
  2. Codex Access Token   : ~/.codex/auth.json -> tokens.access_token
  3. Codex Refresh Token  : ~/.codex/auth.json -> tokens.refresh_token

用法: python get_tokens.py
"""
import json
import os
import sys
from pathlib import Path

HOME = Path.home()


def mask(s: str, keep: int = 12) -> str:
    return s[:keep] + "..." + s[-6:] if len(s) > keep + 6 else s


def read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"  [!] 读取 {p} 失败: {e}")
        return None


def claude_token() -> str | None:
    for p in (HOME / ".claude" / ".credentials.json",
              Path(os.environ.get("CLAUDE_CONFIG_DIR", "")) / ".credentials.json"
              if os.environ.get("CLAUDE_CONFIG_DIR") else None):
        if p is None:
            continue
        data = read_json(p)
        if not data:
            continue
        oauth = data.get("claudeAiOauth") or data.get("oauth") or {}
        tok = oauth.get("accessToken") or data.get("accessToken")
        if tok:
            print(f"  (来源: {p})")
            return tok
    return None


def codex_tokens():
    codex_home = Path(os.environ.get("CODEX_HOME", HOME / ".codex"))
    data = read_json(codex_home / "auth.json")
    if not data:
        return None, None
    toks = data.get("tokens") or {}
    print(f"  (来源: {codex_home / 'auth.json'})")
    return toks.get("access_token"), toks.get("refresh_token")


def main():
    print("=" * 60)
    print("QuotaTV 凭据提取")
    print("=" * 60)

    print("\n[1/3] Claude OAuth Token")
    ct = claude_token()
    if ct:
        print(f"  {ct}")
        if not ct.startswith("sk-ant-oat"):
            print("  [!] 注意: 这是 Claude Code 的会话 access token, 会较快过期。")
            print("      更稳的做法: 终端运行 `claude setup-token` 生成长期 token,")
            print("      把输出的 sk-ant-oat01-... 填进配置页。")
    else:
        print("  [x] 未找到。请在终端运行: claude setup-token")
        print("      然后将输出的 sk-ant-oat01-... 填入配置页。")

    print("\n[2/3] & [3/3] Codex Tokens")
    at, rt = codex_tokens()
    if at:
        print(f"  access_token : {at}")
    else:
        print("  [x] 未找到 access_token。先运行一次 `codex` 并登录 ChatGPT 账号。")
    if rt:
        print(f"  refresh_token: {rt}")
    else:
        print("  [x] 未找到 refresh_token (没有它, access token 过期后需重新粘贴)。")

    print("\n完成。将以上值粘贴到 QuotaTV 配置页 (http://192.168.4.1)。")
    print("注意: 这些是账号凭据, 不要截图外传。")


if __name__ == "__main__":
    sys.exit(main())
