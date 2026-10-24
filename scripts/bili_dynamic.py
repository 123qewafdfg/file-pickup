#!/usr/bin/env python3
"""藏笺 PC · 哔哩哔哩发放（完全照搬安卓版 BiliApi.kt 的协议）

协议来源：android/app/src/main/java/com/trustmark/share/BiliApi.kt
  · 上传  POST api.bilibili.com/x/dynamic/feed/draw/upload_bfs?csrf=<bili_jct>
          multipart 字段名 file_up，Origin/Referer 必须是 member.bilibili.com
  · 发布  POST api.bilibili.com/x/dynamic/feed/create/opus?csrf=..&w_rid=..&wts=..
          需要 WBI 签名（nav 里的 img_url / sub_url + 固定混淆表）
          标题必须是 5 个字母，private_pub=1（私密投稿），scene=12
  · 登录  扫码 passport.bilibili.com qrcode/generate + poll（cookie 从 Set-Cookie 取）

用法：
  python bili_dynamic.py --json status
  python bili_dynamic.py --json login --cookie "SESSDATA=..; bili_jct=..; DedeUserID=.."
  python bili_dynamic.py --json login --qr          # 只取二维码，不轮询
  python bili_dynamic.py --json poll --key <qrcode_key>
  python bili_dynamic.py --json post --file a.png --file b.png --title ABCDE

输出：每行一个 JSON 事件
  {"event":"qr","url":"https://...","key":"..."}
  {"event":"poll","state":"waiting|scanned|expired|confirmed"}
  {"event":"status","logged_in":true,"uid":"...","name":"..."}
  {"event":"slice","index":0,"done":1048576,"total":5243916}
  {"event":"uploaded","index":0,"url":"https://i0.hdslb.com/..."}
  {"event":"posted","urls":[...],"opusId":"..."}
  {"event":"fail","message":"..."}
"""
from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
import ctypes.wintypes as wt
import hashlib
import http.client
import json
import os
import random
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

from concurrent.futures import ThreadPoolExecutor, as_completed

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")
SESSION_PATH = os.path.join(os.environ.get("APPDATA", ""), "Cangjian", "藏笺", "bili-session.dat")
COOKIE_ENV = "CJ_BILI_COOKIE"

QR_GENERATE = "https://passport.bilibili.com/x/passport-login/web/qrcode/generate"
QR_POLL = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll"
NAV = "https://api.bilibili.com/x/web-interface/nav"
UPLOAD = "https://api.bilibili.com/x/dynamic/feed/draw/upload_bfs"
PUBLISH = "https://api.bilibili.com/x/dynamic/feed/create/opus"
MEMBER_REFERER = "https://member.bilibili.com/platform/upload/text"

EDITOR_VERSION = "eva3-4.0.0"
MIXIN_KEY_ENC_TAB = [46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
                     33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
                     26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36,
                     20, 34, 44, 52]


class BiliAuthRequired(RuntimeError):
    pass


# ------------------------------------------------------------ DPAPI 会话

class _Blob(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def dpapi_unprotect(data: bytes) -> bytes:
    buf = ctypes.create_string_buffer(data, len(data))
    blob_in = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _Blob()
    ok = ctypes.windll.crypt32.CryptUnprotectData(
        ctypes.byref(blob_in), None, None, None, None, 0x1, ctypes.byref(blob_out))
    if not ok:
        raise RuntimeError("会话解密失败（文件不属于当前用户）")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


def dpapi_protect(data: bytes) -> bytes:
    buf = ctypes.create_string_buffer(data, len(data))
    blob_in = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _Blob()
    ok = ctypes.windll.crypt32.CryptProtectData(
        ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out))
    if not ok:
        raise RuntimeError("会话加密失败")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


def save_cookie(cookie: str) -> None:
    os.makedirs(os.path.dirname(SESSION_PATH), exist_ok=True)
    tmp = SESSION_PATH + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(dpapi_protect(cookie.encode("utf-8")))
    os.replace(tmp, SESSION_PATH)


def load_cookie() -> str:
    env = os.environ.get(COOKIE_ENV, "").strip()
    if env:
        return env
    if not os.path.exists(SESSION_PATH):
        raise BiliAuthRequired("还没登录哔哩哔哩")
    return dpapi_unprotect(open(SESSION_PATH, "rb").read()).decode("utf-8").strip()


def cookie_value(cookie: str, key: str) -> str:
    for part in cookie.split(";"):
        if "=" in part:
            name, value = part.split("=", 1)
            if name.strip() == key:
                return value.strip()
    return ""


# ------------------------------------------------------------ HTTP

def _open(url: str, data: bytes | None = None, headers: dict | None = None,
          timeout: int = 120):
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET")
    req.add_header("User-Agent", UA)
    req.add_header("Referer", "https://www.bilibili.com/")
    req.add_header("Origin", "https://www.bilibili.com")
    for name, value in (headers or {}).items():
        req.add_header(name, value)
    return urllib.request.urlopen(req, timeout=timeout)


def _json(url: str, data: bytes | None = None, headers: dict | None = None) -> tuple[dict, str]:
    with _open(url, data, headers) as resp:
        raw = resp.read(1024 * 1024)
        set_cookie = resp.headers.get_all("Set-Cookie") or []
    return _parse_json_response(url, raw, set_cookie)


def _parse_json_response(url: str, raw: bytes, set_cookie: list[str] | None = None) -> tuple[dict, str]:
    text = raw.decode("utf-8", "replace")
    try:
        envelope = json.loads(text)
    except Exception:  # noqa: BLE001
        raise RuntimeError(f"B站响应格式错误：{text[:120]}")
    code = envelope.get("code", -999)
    if code in (-101, -111):
        raise BiliAuthRequired("B站登录已失效，请重新登录")
    if code == 37109:
        raise RuntimeError("今日专栏额度已用完（Lv1-3 每天 1 篇），明天再试")
    if code == -352 or code == -412:
        raise RuntimeError("B站风控拦截，请稍后再试或更换网络")
    if code != 0:
        message = str(envelope.get("message") or envelope.get("msg") or "").replace("\n", " ")[:160]
        raise RuntimeError(f"B站拒绝请求（code={code}，stage={urllib.parse.urlparse(url).path}）：{message}")
    return envelope.get("data") or {}, "|".join(set_cookie or [])


def _multipart(field: str, filename: str, blob: bytes, content_type: str) -> tuple[bytes, str]:
    boundary = "----cangjian" + uuid.uuid4().hex
    head = (f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
            f"Content-Type: {content_type}\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    return head + blob + tail, boundary


# ------------------------------------------------------------ WBI 签名

def _wbi_key(url: str) -> str:
    return url.rsplit("/", 1)[-1].split(".")[0]


def wbi_sign(params: dict, img_key: str, sub_key: str) -> dict:
    source = img_key + sub_key
    mixin = "".join(source[i] for i in MIXIN_KEY_ENC_TAB if i < len(source))[:32]
    if len(mixin) != 32:
        raise RuntimeError("B站 WBI 混淆密钥无效")
    wts = str(int(time.time()))
    unsigned = dict(params)
    unsigned["wts"] = wts
    query = "&".join(f"{urllib.parse.quote(k, safe='')}={urllib.parse.quote(str(unsigned[k]), safe='')}"
                     for k in sorted(unsigned))
    digest = hashlib.md5((query + mixin).encode("utf-8")).hexdigest()
    signed = dict(params)
    signed["w_rid"] = digest
    signed["wts"] = wts
    return signed


# ------------------------------------------------------------ 业务

def normalize_image_url(raw: str) -> str:
    parsed = urllib.parse.urlparse(raw)
    host = parsed.hostname or ""
    if not any(host == d or host.endswith("." + d) for d in ("biliimg.com", "hdslb.com")):
        raise RuntimeError(f"B站返回的图片域名不符合预期：{host}")
    return urllib.parse.urlunparse(("https", parsed.netloc, parsed.path, "", parsed.query, ""))


def upload_image(cookie: str, path: str, index: int, image_count: int) -> dict:
    csrf = cookie_value(cookie, "bili_jct")
    if not csrf:
        raise BiliAuthRequired("Cookie 里缺少 bili_jct")
    size = os.path.getsize(path)
    if not (1 <= size <= 20 * 1024 * 1024):
        raise RuntimeError(f"载体图大小超出 B站限制（{size} 字节，上限 20 MB）")
    with open(path, "rb") as fh:
        if fh.read(8) != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError("B站承载图必须是 PNG")

    boundary = "----cangjian" + uuid.uuid4().hex
    filename = os.path.basename(path)
    head = (f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file_up"; filename="{filename}"\r\n'
            "Content-Type: image/png\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    url = UPLOAD + "?csrf=" + urllib.parse.quote(csrf)
    parsed = urllib.parse.urlparse(url)
    request_path = parsed.path + ("?" + parsed.query if parsed.query else "")
    conn = http.client.HTTPSConnection(parsed.hostname, parsed.port, timeout=300)
    try:
        conn.putrequest("POST", request_path, skip_accept_encoding=True)
        conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        conn.putheader("Content-Length", str(len(head) + size + len(tail)))
        conn.putheader("User-Agent", UA)
        conn.putheader("Cookie", cookie)
        conn.putheader("Origin", "https://member.bilibili.com")
        conn.putheader("Referer", MEMBER_REFERER)
        conn.endheaders()
        conn.send(head)

        sent = 0
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(256 * 1024)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                emit({"event": "slice", "index": index, "done": sent, "total": size})
        conn.send(tail)
        emit({"event": "verifying", "index": index, "number": index + 1,
              "count": image_count})

        resp = conn.getresponse()
        raw = resp.read(1024 * 1024)
        set_cookie = resp.headers.get_all("Set-Cookie") or []
        if resp.status < 200 or resp.status > 299:
            raise RuntimeError(f"B站上传失败（HTTP {resp.status}）：{raw[:120]!r}")
        data, _ = _parse_json_response(url, raw, set_cookie)
    finally:
        conn.close()
    image_url = normalize_image_url(str(data.get("image_url") or data.get("url") or ""))
    return {"url": image_url,
            "width": int(data.get("image_width") or 0),
            "height": int(data.get("image_height") or 0),
            "sizeKb": float(data.get("img_size") or round(size / 1000.0, 2))}


def publish_opus(cookie: str, images: list[dict], title: str) -> str:
    csrf = cookie_value(cookie, "bili_jct")
    if not csrf:
        raise BiliAuthRequired("Cookie 里缺少 bili_jct")
    if len(title) != 5 or not title.isalpha():
        raise RuntimeError("B站稿件标题必须是 5 个字母")
    nav, _ = _json(NAV, headers={"Cookie": cookie})
    if not nav.get("isLogin"):
        raise BiliAuthRequired("B站登录已失效，请重新登录")
    mid = int(nav.get("mid") or 0)
    if mid <= 0:
        raise RuntimeError("B站账号信息不完整")
    wbi = nav.get("wbi_img") or {}
    img_key, sub_key = _wbi_key(str(wbi.get("img_url", ""))), _wbi_key(str(wbi.get("sub_url", "")))
    upload_id = f"{mid}_{int(time.time())}_{random.randint(0, 9999)}"
    paragraphs = []
    for item in images:
        paragraphs.append({"para_type": 2,
                           "pic": {"pics": [{"url": item["url"], "width": item["width"],
                                             "height": item["height"], "size": item["sizeKb"]}],
                                   "style": 0}})
    payload = {
        "raw_content": "",
        "opus_req": {
            "upload_id": upload_id,
            "opus": {
                "opus_source": 2,
                "title": title,
                "content": {"paragraphs": paragraphs},
                "article": {"category_id": 15, "list_id": 0, "originality": 0, "reproduced": 1},
                "pub_info": {"editor_version": EDITOR_VERSION},
            },
            "scene": 12,
            "meta": {"app_meta": {"from": "create.creative.h5", "mobi_app": "web"}},
            "option": {"aigc": 2, "close_comment": 0, "up_choose_comment": 0, "private_pub": 1},
        },
    }
    signed = wbi_sign({"gaia_source": "main_web", "w_opus_req.upload_id": upload_id},
                      img_key, sub_key)
    query = {"csrf": csrf, **signed}
    url = PUBLISH + "?" + urllib.parse.urlencode(query)
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    data, _ = _json(url, body, {
        "Cookie": cookie,
        "Content-Type": "application/json; charset=utf-8",
        "Origin": "https://member.bilibili.com",
        "Referer": MEMBER_REFERER,
    })
    opus_id = str(data.get("dyn_id_str") or data.get("dyn_id") or "")
    return opus_id


# ------------------------------------------------------------ 命令

def emit(payload: dict) -> None:
    print(json.dumps(payload, ensure_ascii=False), flush=True)


def cmd_status() -> int:
    try:
        cookie = load_cookie()
    except BiliAuthRequired as exc:
        emit({"event": "status", "logged_in": False, "message": str(exc)})
        return 0
    try:
        nav, _ = _json(NAV, headers={"Cookie": cookie})
    except BiliAuthRequired:
        emit({"event": "status", "logged_in": False, "message": "登录已失效"})
        return 0
    except Exception as exc:  # noqa: BLE001
        emit({"event": "status", "logged_in": False, "message": str(exc)})
        return 0
    if not nav.get("isLogin"):
        emit({"event": "status", "logged_in": False, "message": "未登录"})
        return 0
    level = (nav.get("level_info") or {}).get("current_level", -1)
    emit({"event": "status", "logged_in": True, "uid": str(nav.get("mid")),
          "name": nav.get("uname", ""), "level": level})
    return 0


def cmd_login(cookie: str) -> int:
    cookie = cookie.strip()
    if not cookie:
        emit({"event": "fail", "message": "Cookie 为空"})
        return 2
    if not cookie_value(cookie, "SESSDATA") or not cookie_value(cookie, "bili_jct"):
        emit({"event": "fail", "message": "Cookie 需要同时包含 SESSDATA 与 bili_jct"})
        return 2
    save_cookie(cookie)
    emit({"event": "login", "ok": True, "uid": cookie_value(cookie, "DedeUserID")})
    return 0


def _segno():
    try:
        import segno
        return segno
    except ImportError:
        for extra in (r"E:\pz\packages\python\segno",):
            if extra not in sys.path:
                sys.path.insert(0, extra)
        import segno
        return segno


def qr_png(url: str) -> str:
    cache = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Cangjian", "藏笺", "cache")
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, "bili-qr.png")
    _segno().make(url, error="m").save(path, scale=8, border=2)
    return path


def cmd_qr(wait: bool) -> int:
    data, _ = _json(QR_GENERATE)
    url, key = data.get("url", ""), data.get("qrcode_key", "")
    if not url.startswith("https://") or not key:
        emit({"event": "fail", "message": "B站二维码无效"})
        return 2
    try:
        png = qr_png(url)
    except Exception as exc:  # noqa: BLE001
        emit({"event": "fail", "message": f"二维码生成失败：{exc}"})
        return 2
    emit({"event": "qr", "url": url, "key": key, "png": png})
    if not wait:
        return 0
    deadline = time.time() + 180
    while time.time() < deadline:
        time.sleep(2)
        try:
            data, set_cookie = _json(QR_POLL + "?" + urllib.parse.urlencode({"qrcode_key": key}))
        except Exception as exc:  # noqa: BLE001
            emit({"event": "fail", "message": f"轮询失败：{exc}"})
            return 3
        state = {86101: "waiting", 86090: "scanned", 86038: "expired",
                 0: "confirmed"}.get(int(data.get("code", -1)))
        if state is None:
            emit({"event": "fail", "message": f"未知扫码状态 {data.get('code')}"})
            return 3
        if state == "confirmed":
            pairs: dict[str, str] = {}
            for chunk in set_cookie.split("|"):
                head = chunk.split(";", 1)[0].strip()
                if "=" not in head:
                    continue
                name, value = head.split("=", 1)
                if name.strip().lower() in ("expires", "path", "domain", "max-age", "samesite",
                                            "secure", "httponly"):
                    continue
                pairs[name.strip()] = value
            cookie = "; ".join(f"{k}={v}" for k, v in pairs.items())
            if cookie:
                save_cookie(cookie)
            emit({"event": "poll", "state": state, "uid": cookie_value(cookie, "DedeUserID")})
            return 0
        emit({"event": "poll", "state": state})
        if state == "expired":
            return 0
    emit({"event": "poll", "state": "timeout"})
    return 0


def cmd_poll(key: str) -> int:
    url = QR_POLL + "?" + urllib.parse.urlencode({"qrcode_key": key})
    data, set_cookie = _json(url)
    code = int(data.get("code", -1))
    state = {86101: "waiting", 86090: "scanned", 86038: "expired", 0: "confirmed"}.get(code)
    if state is None:
        emit({"event": "fail", "message": f"B站返回未知扫码状态 {code}"})
        return 2
    if state == "confirmed":
        pairs: dict[str, str] = {}
        for chunk in set_cookie.split("|"):
            head = chunk.split(";", 1)[0].strip()
            if "=" not in head:
                continue
            name, value = head.split("=", 1)
            if name.strip().lower() in ("expires", "path", "domain", "max-age", "samesite",
                                        "secure", "httponly"):
                continue
            pairs[name.strip()] = value
        cookie = "; ".join(f"{k}={v}" for k, v in pairs.items())
        if cookie:
            save_cookie(cookie)
        emit({"event": "poll", "state": state, "uid": cookie_value(cookie, "DedeUserID")})
    else:
        emit({"event": "poll", "state": state})
    return 0


UPLOAD_WORKERS = 4      # 并发上传数：单连接实测只有 ~75 KB/s，串行 5 张要 3 分钟
UPLOAD_RETRIES = 3      # 每张重试次数：SSL EOF / DNS 抖动很常见


def upload_with_retry(cookie: str, path: str, index: int, total: int) -> dict:
    last: Exception | None = None
    for attempt in range(1, UPLOAD_RETRIES + 1):
        try:
            return upload_image(cookie, path, index, total)
        except BiliAuthRequired:
            raise
        except Exception as exc:  # noqa: BLE001
            last = exc
            if attempt < UPLOAD_RETRIES:
                time.sleep(1.5 * attempt)
    raise RuntimeError(f"重试 {UPLOAD_RETRIES} 次仍失败：{last}")


def cmd_post(files: list[str], title: str, publish: bool = True) -> int:
    try:
        cookie = load_cookie()
    except BiliAuthRequired as exc:
        emit({"event": "fail", "message": str(exc)})
        return 2
    sizes = [os.path.getsize(f) for f in files]
    images: list[dict | None] = [None] * len(files)
    lock = threading.Lock()
    failures: list[str] = []

    emit({"event": "uploading", "done": 0, "total": len(files)})
    for index, size in enumerate(sizes):
        emit({"event": "slice", "index": index, "done": 0, "total": size})

    def worker(index: int, path: str) -> None:
        try:
            item = upload_with_retry(cookie, path, index, len(files))
        except Exception as exc:  # noqa: BLE001
            with lock:
                failures.append(f"第 {index + 1} 张：{exc}")
                emit({"event": "fail", "message": f"第 {index + 1} 张：{exc}"})
            return
        images[index] = item
        with lock:
            done = sum(1 for entry in images if entry is not None)
            emit({"event": "uploaded", "index": index, "url": item["url"],
                  "done": done, "total": len(files)})

    workers = max(1, min(UPLOAD_WORKERS, len(files)))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(worker, index, path) for index, path in enumerate(files)]
        for future in as_completed(futures):
            future.result()
    if failures:
        return 3
    images = [item for item in images if item is not None]
    if not publish:
        emit({"event": "posted", "urls": [item["url"] for item in images],
              "published": False, "opusId": "", "url": ""})
        return 0
    emit({"event": "publishing", "platform": "bilibili"})
    try:
        opus_id = publish_opus(cookie, images, title)
    except Exception as exc:  # noqa: BLE001
        emit({"event": "fail", "message": str(exc)})
        return 4
    emit({"event": "posted", "urls": [item["url"] for item in images],
          "published": True, "opusId": opus_id,
          "url": f"https://www.bilibili.com/opus/{opus_id}" if opus_id else ""})
    return 0


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    parser = argparse.ArgumentParser(description="藏笺 · 哔哩哔哩发放")
    parser.add_argument("command", choices=["status", "login", "qr", "poll", "post"])
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--title", default="")
    parser.add_argument("--cookie", default="")
    parser.add_argument("--key", default="")
    parser.add_argument("--wait", action="store_true")
    parser.add_argument("--no-publish", action="store_true",
                        help="只上传并返回直链，不发布动态")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.command == "status":
        return cmd_status()
    if args.command == "login":
        return cmd_login(args.cookie)
    if args.command == "qr":
        return cmd_qr(wait=args.wait)
    if args.command == "poll":
        return cmd_poll(args.key)
    if not args.file:
        emit({"event": "fail", "message": "没有要上传的文件"})
        return 2
    title = args.title or "".join(random.choice("ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(5))
    return cmd_post(args.file, title, publish=not args.no_publish)


if __name__ == "__main__":
    sys.exit(main())
