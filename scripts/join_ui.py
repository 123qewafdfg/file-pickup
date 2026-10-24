# -*- coding: utf-8 -*-
"""用 CDP 在已登录的 Edge 里点“加入频道”（页面自己生成 turing 签名）。

用法：
  python join_ui.py            # 点击加入并等待结果
  python join_ui.py --dry      # 只看页面状态，不点击
  python join_ui.py --verify   # 只做成员状态核验（调接口）
"""
import json, os, sys, time, urllib.request
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)
from cdp_capture import WS

PORT = 9333
EDGE_CANDIDATES = (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                   r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")


def _port_ready(timeout=3):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/json/version" % PORT, timeout=timeout):
            return True
    except Exception:
        return False


def ensure_browser(timeout=90):
    """没有可用的调试端口就自己拉起 Edge（复用已登录的配置目录）。"""
    if _port_ready():
        return
    import os, subprocess
    profile = os.environ.get("CJ_EDGE_PROFILE", r"E:\pz\qq-channel-edge-profile")
    for exe in EDGE_CANDIDATES:
        if os.path.exists(exe):
            subprocess.Popen([exe, "--remote-debugging-port=%d" % PORT,
                              "--user-data-dir=" + profile, "--no-first-run",
                              "--no-default-browser-check",
                              "https://pd.qq.com/g/pd55235600"])
            break
    end = time.time() + timeout
    while time.time() < end:
        if _port_ready():
            time.sleep(4)   # 等页面脚本加载出加入按钮
            return
        time.sleep(1.5)
    raise SystemExit("Edge 调试端口没起来")


def page_target():
    with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % PORT, timeout=10) as r:
        for t in json.load(r):
            if t.get("type") == "page" and "pd.qq.com" in (t.get("url") or ""):
                return t
    raise SystemExit("找不到已打开的频道页")


class Client:
    def __init__(self):
        self.ws = WS(page_target()["webSocketDebuggerUrl"])
        self.n = 0

    def call(self, method, params=None):
        self.n += 1
        mid = self.n
        self.ws.send({"id": mid, "method": method, "params": params or {}})
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == mid:
                return msg

    def js(self, expr):
        r = self.call("Runtime.evaluate", {"expression": expr, "returnByValue": True,
                                           "awaitPromise": True})
        res = (r.get("result") or {}).get("result", {})
        if res.get("subtype") == "error":
            return "JS_ERROR: " + str(res.get("description"))[:200]
        return res.get("value")


CLICK_JOIN = r"""
(() => {
  const btn = document.querySelector('.join-guild-button, .top_title_join_btn');
  if (!btn) return 'no-join-button';
  const r = btn.getBoundingClientRect();
  if (r.width === 0 || r.height === 0) return 'button-invisible';
  btn.click();
  return 'clicked:' + (btn.textContent || '').trim();
})()
"""

CLICK_CONFIRM = r"""
(() => {
  const wanted = ['确定', '确认', '加入频道', '同意', '申请加入'];
  const out = [];
  for (const el of document.querySelectorAll('button, .q-button, .dialog-button, [class*=btn]')) {
    const t = (el.textContent || '').trim();
    if (wanted.includes(t)) {
      el.click();
      out.push(t);
      if (out.length >= 1) break;
    }
  }
  return out.join(',') || 'no-confirm-button';
})()
"""


def web_status():
    """调接口判断：已加入时 join 返回 0（空操作），未加入返回 22003。"""
    import qq_channel as qc
    cookie = qc.load_cookies()
    bkn = qc._bkn(qc._cookie_value(cookie, "p_skey"))
    r = qc.join_guild(cookie, bkn)
    return r.get("retcode"), (r.get("message") or "")[:60]


def emit(payload):
    print(json.dumps(payload, ensure_ascii=False), flush=True)


def main():
    dry = "--dry" in sys.argv
    verify_only = "--verify" in sys.argv
    json_mode = "--json" in sys.argv

    def report(payload, text):
        if json_mode:
            emit(payload)
        else:
            print(text)

    if verify_only:
        code, message = web_status()
        report({"event": "joined", "joined": code == 0, "retcode": code,
                "message": message}, f"接口判定: {(code, message)}")
        return 0 if code == 0 else 3

    before, before_message = web_status()
    if before == 0:
        report({"event": "joined", "joined": True, "already": True}, "已经加入频道")
        return 0
    if before != 22003:
        report({"event": "fail", "message": f"无法确认频道成员状态：{before} {before_message}"},
               f"无法确认频道成员状态: {before} {before_message}")
        return 2

    ensure_browser()
    c = Client()
    title = c.js("document.title")
    report({"event": "page", "title": title}, f"页面: {title}")

    if dry:
        exists = c.js("!!document.querySelector('.join-guild-button, .top_title_join_btn')")
        report({"event": "dry", "button": bool(exists)}, f"加入按钮存在: {exists}")
        return 0

    clicked = c.js(CLICK_JOIN)
    report({"event": "click", "result": clicked}, f"点击: {clicked}")
    time.sleep(2.5)
    confirmed = c.js(CLICK_CONFIRM)
    report({"event": "confirm", "result": confirmed}, f"二次确认: {confirmed}")
    time.sleep(3.0)
    after, message = web_status()
    joined = after == 0
    report({"event": "joined" if joined else "fail", "joined": joined,
            "retcode": after, "message": message or ("已加入频道" if joined else "自动加入失败")},
           f"点击后接口判定: {(after, message)}")
    return 0 if joined else 3


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        if "--json" in sys.argv:
            emit({"event": "fail", "message": str(exc)})
        else:
            print("自动加入失败:", exc, file=sys.stderr)
        sys.exit(2)
