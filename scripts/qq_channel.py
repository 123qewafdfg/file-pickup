#!/usr/bin/env python3
"""藏笺 PC · QQ 频道上传器（Python 版）

为什么用脚本：频道上传/发帖这一段协议变来变去，用 Python 改起来比重编 C++ 快得多。
C++ 只负责加密和生成载体，上传交给这个脚本，进度按 JSON 行回流给界面。

用法：
  python qq_channel.py status                     # 只检查登录态（不联网上传）
  python qq_channel.py upload --file a.png --file b.png [--json]
  python qq_channel.py check --url https://...    # HEAD 校验直链

输出：每行一个 JSON（--json 时）或人类可读文本；进度事件形如
  {"event":"progress","index":0,"sent":1048576,"total":5243916,"percent":20,"overall":11}
"""
from __future__ import annotations

import argparse
import base64
import base64
import ctypes
import ctypes.wintypes as wt
import hashlib
import http.client
import json
import os
import random
import re
import ssl
import struct
import sys
import threading
import time

UPLOAD_HOST = "pd.qq.com"
UPLOAD_PATH = "/qunng/guild/gotrpc/v1/trpc.qqntv2.richmedia.InnerProxy/Upload"
GUILD_ID = "16670831792565266"
CHANNEL_ID = "744499928"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")
SESSION_PATH = os.path.join(os.environ.get("APPDATA", ""), "Cangjian", "藏笺", "qq-session.dat")

# ---------------------------------------------------------------- DPAPI 解密


class _Blob(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def dpapi_unprotect(data: bytes) -> bytes:
    buf = ctypes.create_string_buffer(data, len(data))
    blob_in = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _Blob()
    ok = ctypes.windll.crypt32.CryptUnprotectData(
        ctypes.byref(blob_in), None, None, None, None, 0x1, ctypes.byref(blob_out))
    if not ok:
        raise RuntimeError("DPAPI 解密失败（会话文件不属于当前用户？）")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


def load_cookies(path: str = SESSION_PATH) -> str:
    if not os.path.exists(path):
        raise RuntimeError(f"找不到会话文件：{path}（先在应用里扫码登录）")
    with open(path, "rb") as fh:
        raw = dpapi_unprotect(fh.read())
    items = json.loads(raw.decode("utf-8"))
    pairs = [f"{c['name']}={c['value']}" for c in items if c.get("name") and c.get("value")]
    if not pairs:
        raise RuntimeError("会话文件里没有 cookie")
    return "; ".join(pairs)


# ---------------------------------------------------------------- 上传


def _multipart_head(boundary: str, filename: str) -> bytes:
    parts = []
    for name, value in (("guild_id", GUILD_ID), ("channel_id", CHANNEL_ID)):
        parts.append(f"--{boundary}\r\n"
                     f'Content-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n')
    parts.append(f"--{boundary}\r\n"
                 f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
                 f"Content-Type: image/png\r\n\r\n")
    return "".join(parts).encode("utf-8")


def _multipart_tail(boundary: str) -> bytes:
    return f"\r\n--{boundary}--\r\n".encode("utf-8")


def _direct_url(compat_b64: str) -> str | None:
    """compat_qq_msg 是 base64 的 protobuf，原图那一档是 .../0?term=..."""
    try:
        bin_data = base64.b64decode(compat_b64)
    except Exception:
        return None
    found = re.findall(rb"qmeetpic/\d+/[\w-]+/\d+\?term=\d+", bin_data)
    if not found:
        return None
    paths = [f.decode("ascii") for f in found]
    for p in paths:
        if re.search(r"/0\?term=", p):
            return "https://gchat.qpic.cn/" + p
    return "https://gchat.qpic.cn/" + paths[0]


class Progress:
    def __init__(self, count: int, emit):
        self._lock = threading.Lock()
        self._sent = [0] * count
        self._total = [0] * count
        self._emit = emit

    def update(self, index: int, sent: int, total: int) -> None:
        with self._lock:
            self._sent[index] = sent
            self._total[index] = total
            overall_sent = sum(self._sent)
            overall_total = sum(self._total)
            overall = int(overall_sent * 100 / overall_total) if overall_total else 0
            percent = int(sent * 100 / total) if total else 0
        self._emit({"event": "progress", "index": index, "sent": sent,
                    "total": total, "percent": percent, "overall": overall})


def upload_one(path: str, index: int, cookie: str, progress: Progress, emit, timeout: int = 300) -> str:
    filename = os.path.basename(path)
    size = os.path.getsize(path)
    if size <= 0:
        raise RuntimeError(f"{filename} 是空文件")
    if size > 30 * 1024 * 1024:
        raise RuntimeError(f"{filename} 超过 30 MB 上限（{size} 字节）")

    boundary = f"----cangjian{int(time.time() * 1000)}{index}"
    head = _multipart_head(boundary, filename)
    tail = _multipart_tail(boundary)
    total = len(head) + size + len(tail)

    conn = http.client.HTTPSConnection(UPLOAD_HOST, timeout=timeout)
    try:
        conn.putrequest("POST", UPLOAD_PATH, skip_accept_encoding=True)
        conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        conn.putheader("Content-Length", str(total))
        conn.putheader("User-Agent", UA)
        conn.putheader("Origin", "https://pd.qq.com")
        conn.putheader("Referer", "https://pd.qq.com/g/pd55235600")
        conn.putheader("Cookie", cookie)
        conn.endheaders()

        sent = 0
        conn.send(head)
        sent += len(head)
        progress.update(index, sent, total)

        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(1024 * 1024)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                progress.update(index, sent, total)

        conn.send(tail)
        sent += len(tail)
        progress.update(index, sent, total)

        resp = conn.getresponse()
        body = resp.read()
        status = resp.status
    finally:
        conn.close()

    if status != 200:
        raise RuntimeError(f"上传 HTTP {status}：{body[:200]!r}")
    try:
        data = json.loads(body.decode("utf-8"))
    except Exception:
        raise RuntimeError(f"上传响应不是 JSON：{body[:200]!r}")
    if data.get("retcode", 0) != 0:
        raise RuntimeError(f"上传被拒 retcode={data.get('retcode')} {data.get('message', '')}")
    compat = (data.get("data") or {}).get("compat_qq_msg", "")
    url = _direct_url(compat) if compat else None
    if not url:
        raise RuntimeError("上传成功但没解析出直链（compat_qq_msg 结构变了）")
    emit({"event": "uploaded", "index": index, "url": url, "bytes": size})
    return url


def cmd_upload(args, emit) -> int:
    cookie = load_cookies(args.session)
    files = args.file
    count = len(files)
    progress = Progress(count, emit)
    urls: list[str | None] = [None] * count
    errors: list[str] = []
    lock = threading.Lock()

    def worker(i: int) -> None:
        try:
            urls[i] = upload_one(files[i], i, cookie, progress, emit)
        except Exception as exc:  # noqa: BLE001
            with lock:
                errors.append(f"[{i}] {exc}")
            emit({"event": "error", "index": i, "message": str(exc)})

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(count)]
    started = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    if errors:
        emit({"event": "fail", "message": "; ".join(errors)})
        return 1
    emit({"event": "done", "urls": urls, "elapsed": round(time.time() - started, 1)})
    return 0


# ---------------------------------------------------------------- 其他命令


def cmd_status(args, emit) -> int:
    cookie = load_cookies(args.session)
    names = sorted({p.split("=", 1)[0] for p in cookie.split("; ")})
    emit({"event": "status", "session": args.session,
          "cookies": len(names), "has_p_skey": "p_skey" in names,
          "has_uin": "uin" in names, "names": names})
    return 0


def cmd_check(args, emit) -> int:
    from urllib.request import Request, urlopen

    req = Request(args.url, method="HEAD", headers={"Referer": "https://pd.qq.com/", "User-Agent": UA})
    with urlopen(req, timeout=30) as resp:
        emit({"event": "check", "status": resp.status,
              "type": resp.headers.get("Content-Type"),
              "length": resp.headers.get("Content-Length")})
    return 0



# ---------------------------------------------------------------- 发帖（分片上传）

SITE = "pd.qq.com"
GUILD_ID = "16670831792565266"
CHANNEL_ID = "744499928"
RICH_APPID = 1002
SLICE = 1048576
MAX_BYTES = 30 * 1024 * 1024
MAX_IMAGES = 18


def _bkn(skey: str) -> int:
    h = 5381
    for ch in skey:
        h += (h << 5) + ord(ch)
    return h & 0x7FFFFFFF


def _cookie_value(cookie: str, name: str) -> str:
    for part in cookie.split(";"):
        part = part.strip()
        if part.startswith(name + "="):
            return part[len(name) + 1:]
    return ""


def _pd(method: str, path: str, body: bytes | None, cookie: str,
        headers: dict | None, timeout: int = 300) -> tuple[int, bytes]:
    conn = http.client.HTTPSConnection(SITE, context=ssl.create_default_context(), timeout=timeout)
    hd = {"Cookie": cookie, "X-QQ-Client-AppId": "537246381",
          "Origin": "https://pd.qq.com", "Referer": "https://pd.qq.com/", "User-Agent": UA}
    if headers:
        hd.update(headers)
    try:
        conn.request(method, path, body=body, headers=hd)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


_TLS = ssl.create_default_context()
_LOCAL = threading.local()


def _conn() -> http.client.HTTPSConnection:
    """线程内复用同一条 TLS 连接：分片上传最怕每片重握手。"""
    conn = getattr(_LOCAL, "conn", None)
    if conn is None:
        conn = http.client.HTTPSConnection(SITE, context=_TLS, timeout=300)
        _LOCAL.conn = conn
    return conn


def _post_keepalive(path: str, body: bytes, cookie: str, headers: dict) -> tuple[int, bytes]:
    hd = {"Cookie": cookie, "X-QQ-Client-AppId": "537246381",
          "Origin": "https://pd.qq.com", "Referer": "https://pd.qq.com/", "User-Agent": UA}
    hd.update(headers)
    last: Exception | None = None
    for _ in range(3):
        conn = _conn()
        try:
            conn.request("POST", path, body=body, headers=hd)
            resp = conn.getresponse()
            return resp.status, resp.read()
        except (http.client.HTTPException, OSError) as exc:  # 连接被服务端关掉就重连一次
            last = exc
            try:
                conn.close()
            except Exception:
                pass
            _LOCAL.conn = None
    raise last if last else RuntimeError("请求失败")


def png_size(path: str) -> tuple[int, int]:
    with open(path, "rb") as fh:
        head = fh.read(33)
    if len(head) >= 24 and head[:8] == b"\x89PNG\r\n\x1a\n":
        return struct.unpack(">II", head[16:24])
    return (100, 100)


# ---------------------------------------------------------------- 上传调度与计时

RATE_LIMIT_CODES = {10015, 4000, 20000}


def _now() -> float:
    return time.monotonic()


class UploadError(Exception):
    def __init__(self, message: str, code: int | None = None, *, retryable: bool = False,
                 rate_limited: bool = False, fatal: bool = False):
        super().__init__(message)
        self.code = code
        self.retryable = retryable
        self.rate_limited = rate_limited
        self.fatal = fatal


class FileScheduler:
    """同账号共享的文件级并发闸门：降低上限只影响领取新文件。"""

    def __init__(self, limit: int = 2):
        self._cond = threading.Condition()
        self._limit = max(1, limit)
        self._active = 0
        self.degraded = False
        self.reason = ""

    @property
    def effective_limit(self) -> int:
        return 1 if self.degraded else self._limit

    def acquire(self) -> None:
        with self._cond:
            while self._active >= self.effective_limit:
                self._cond.wait()
            self._active += 1

    def release(self) -> None:
        with self._cond:
            self._active -= 1
            self._cond.notify_all()

    def degrade(self, reason: str) -> bool:
        with self._cond:
            if self.degraded:
                return False
            self.degraded = True
            self.reason = reason
            self._cond.notify_all()
            return True


def _log(emit, event: str, **fields) -> None:
    payload = {"event": event, "t": round(time.time(), 3)}
    payload.update(fields)
    if emit:
        emit(payload)


def _backoff(attempt: int) -> float:
    wait = min(4.0, 1.0 * (2 ** attempt)) + random.uniform(0, 0.3)
    time.sleep(wait)
    return wait


def prepare_file(path: str) -> tuple[bytes, str, list[str]]:
    """一次读入 + 一次前进式扫描：整文件 SHA-1 与各片前缀 SHA-1 都不重复计算。"""
    size = os.path.getsize(path)
    if size > MAX_BYTES:
        raise UploadError("图片 %d 字节超过 30 MB 上限" % size, fatal=True)
    data = open(path, "rb").read()
    digest = hashlib.sha1()
    cums: list[str] = []
    view = memoryview(data)
    for start in range(0, size, SLICE):
        digest.update(view[start:start + SLICE])
        cums.append(digest.copy().hexdigest())
    return data, digest.hexdigest(), cums


def _apply_slice_upload(cookie: str, bkn: int, size: int, sha1: str) -> dict:
    payload = json.dumps({"appid": RICH_APPID, "agent_type": 34, "business_type": 1,
                          "size": size, "is_origin": True, "is_try_full_upload": False,
                          "sha1": sha1}).encode()
    st, body = _pd("POST", "/proxy/domain/richmedia.qq.com/ApplySliceUpload?bkn=%d" % bkn,
                   payload, cookie, {"Content-Type": "application/json"})
    info = json.loads(body) if body else {"retcode": st}
    if info.get("retcode") != 0:
        raise UploadError("申请分片失败: %s" % info.get("retmsg"), info.get("retcode"),
                          retryable=info.get("retcode") in RATE_LIMIT_CODES,
                          rate_limited=info.get("retcode") in RATE_LIMIT_CODES)
    return info


def _send_slice(cookie: str, bkn: int, ukey: str, data: bytes, cums: list[str],
                index: int, trace: list, max_retries: int) -> dict:
    """单片的唯一重试入口：保持字节、索引、范围、哈希与所属会话一致。"""
    size = len(data)
    start = index * SLICE
    end = min(start + SLICE, size)
    headers = {"X-richmedia-ukey": ukey, "appid": str(RICH_APPID), "business_type": "1",
               "agent_type": "34", "X-richmedia-range-start": str(start),
               "X-richmedia-range-end": str(end - 1),
               "X-richmedia-cumulatesha1": json.dumps(cums[: index + 1]),
               "Content-Type": "application/octet-stream"}
    path = ("/proxy/domain/richmedia.qq.com/UploadSliceData?bkn=%d&ukey=%s&index=%d"
            % (bkn, ukey[:12], index))
    last: UploadError | None = None
    for attempt in range(max_retries + 1):
        began = _now()
        try:
            st, resp = _post_keepalive(path, data[start:end], cookie, headers)
            got = json.loads(resp) if resp else {"retcode": st}
        except (http.client.HTTPException, OSError) as exc:
            spent = round(_now() - began, 2)
            trace.append({"index": index, "attempt": attempt, "sec": spent, "error": type(exc).__name__})
            last = UploadError("分片 %d 连接异常: %s" % (index, exc), retryable=True)
            if attempt < max_retries:
                _backoff(attempt)
                continue
            raise last
        spent = round(_now() - began, 2)
        code = got.get("retcode")
        trace.append({"index": index, "attempt": attempt, "sec": spent, "retcode": code})
        if code == 0:
            return got
        message = got.get("retmsg") or got.get("message") or ""
        retryable = code == -5503005 or bool(got.get("retryable"))
        rate_limited = code in RATE_LIMIT_CODES or "频繁" in message
        last = UploadError("分片 %d 失败(%s): %s" % (index, code, message[:60]),
                           code, retryable=retryable, rate_limited=rate_limited)
        if attempt < max_retries and (retryable or rate_limited):
            _backoff(attempt + (2 if rate_limited else attempt))
            continue
        raise last
    raise last if last else UploadError("分片 %d 失败" % index)


def slice_upload(path: str, cookie: str, bkn: int, emit, index: int = 0,
                 max_retries: int = 3, trace: list | None = None) -> dict:
    """完整上传一个文件：顺序分片，取得最后一帧（含 extend_info）为止。"""
    trace = [] if trace is None else trace
    data, sha1, cums = prepare_file(path)
    size = len(data)
    total = (size + SLICE - 1) // SLICE
    t_apply = _now()
    info = _apply_slice_upload(cookie, bkn, size, sha1)
    apply_sec = round(_now() - t_apply, 2)
    ukey = info["ukey"]
    t_up = _now()
    final: dict = {}
    for i in range(total):
        final = _send_slice(cookie, bkn, ukey, data, cums, i, trace, max_retries)
        if emit:
            sent = min((i + 1) * SLICE, size)
            emit({"event": "progress", "index": index, "sent": sent, "total": size,
                  "percent": round(sent * 100 / size, 1)})
    return {"final": final, "size": size, "slices": total, "sha1": sha1,
            "apply_sec": apply_sec, "upload_sec": round(_now() - t_up, 2),
            "url": channel_image_url(final, size)}


def channel_image_url(final: dict, expect_bytes: int = 0) -> str:
    """从 extend_info 里挑**原图**地址（img_class=1 是缩略图，不可用）。"""
    import base64 as _b64
    raw = final.get("extend_info") or ""
    if not raw:
        return ""
    infos = [ii for ii in (json.loads(_b64.b64decode(raw)).get("img_infos") or [])
             if ii.get("img_url")]
    if not infos:
        return ""
    if expect_bytes:
        for ii in infos:
            if int(ii.get("img_size") or 0) == expect_bytes:
                return ii["img_url"]
    return max(infos, key=lambda ii: int(ii.get("img_size") or 0))["img_url"]


def publish_feed(cookie: str, bkn: int, entries: list[dict], timeout: int = 120) -> dict:
    feed = {"createTime": str(int(time.time())),
            "poster": {"id": _poster_id(cookie)},
            "channelInfo": {"sign": {"guild_id": GUILD_ID, "channel_id": CHANNEL_ID}},
            "contents": {"contents": []},
            "images": entries, "videos": [], "title": {}, "feedType": 1,
            "feed_risk_info": {"declaration_type": 0, "icon_url": "", "risk_content": ""}}
    body = {"feed": feed, "jsonFeed": json.dumps(feed, ensure_ascii=False),
            "client_content": {"clientImageContents": [
                {"task_id": e["task_id"], "url": e["picUrl"]} for e in entries],
                "clientVideoContents": []}}
    st, resp = _pd("POST",
                   "/qunng/guild/gotrpc/auth/trpc.qchannel.commwriter.ComWriter/PublishFeed?bkn=%d" % bkn,
                   json.dumps(body, ensure_ascii=False).encode(), cookie,
                   {"x-oidb": json.dumps({"uint32_service_type": 10}),
                    "content-type": "application/json"}, timeout)
    return json.loads(resp) if resp else {"retcode": st}


def _poster_id(cookie: str) -> str:
    return "144115221088734995"


def cmd_post(args, emit) -> int:
    cookie = load_cookies(args.session)
    bkn = _bkn(_cookie_value(cookie, "p_skey"))
    files = list(args.file)
    count = len(files)
    if count > MAX_IMAGES:
        emit({"event": "fail", "message": f"单帖最多 {MAX_IMAGES} 张图片"})
        return 1

    limit = max(1, min(getattr(args, "concurrency", 2) or 2, MAX_IMAGES))
    scheduler = FileScheduler(limit)
    batch_began = _now()
    results: list[dict | None] = [None] * count
    errors: list[str] = []
    lock = threading.Lock()
    pending = list(range(count))
    pending_lock = threading.Lock()
    total_bytes = 0
    for path in files:
        try:
            total_bytes += os.path.getsize(path)
        except OSError:
            pass

    _log(emit, "batch.start", files=count, bytes=total_bytes, concurrency=limit)

    def worker() -> None:
        while True:
            with pending_lock:
                if not pending:
                    return
                idx = pending.pop(0)
            path = files[idx]
            scheduler.acquire()
            began = _now()
            try:
                trace: list = []
                out = slice_upload(path, cookie, bkn, emit, idx, max_retries=3, trace=trace)
                if not out["url"]:
                    raise UploadError("分片全部成功但没拿到原图地址（上传未完成）", fatal=True)
                w, h = png_size(path)
                entry = {"task_id": str(random.randint(10 ** 16, 10 ** 17 - 1)),
                         "picId": None, "picUrl": out["url"], "width": w, "height": h,
                         "display_index": idx}
                entry["picId"] = entry["task_id"]
                with lock:
                    results[idx] = entry
                _log(emit, "file.ok", index=idx, bytes=out["size"], slices=out["slices"],
                     apply_sec=out["apply_sec"], upload_sec=out["upload_sec"],
                     sec=round(_now() - began, 2), retries=max(0, len(trace) - out["slices"]))
            except UploadError as exc:
                with lock:
                    errors.append("[%d] %s" % (idx, exc))
                _log(emit, "file.fail", index=idx, code=exc.code, message=str(exc)[:120])
                if exc.rate_limited and scheduler.degrade("限流: %s" % exc):
                    _log(emit, "batch.degrade", reason="rate-limit", limit=1)
                elif exc.fatal:
                    pass
                else:
                    with lock:
                        fails = len(errors)
                    if fails >= 2 and scheduler.degrade("连续失败 %d 个文件" % fails):
                        _log(emit, "batch.degrade", reason="consecutive-failures", limit=1)
            finally:
                scheduler.release()

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(min(limit, count))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # 只重试失败文件一次，已成功的保留结果
    failed = [i for i, v in enumerate(results) if v is None]
    if failed and not scheduler.degraded:
        _log(emit, "batch.retry", files=len(failed))
        for idx in failed:
            scheduler.acquire()
            try:
                out = slice_upload(files[idx], cookie, bkn, emit, idx, max_retries=2)
                if out["url"]:
                    w, h = png_size(files[idx])
                    entry = {"task_id": str(random.randint(10 ** 16, 10 ** 17 - 1)),
                             "picUrl": out["url"], "width": w, "height": h, "display_index": idx}
                    entry["picId"] = entry["task_id"]
                    results[idx] = entry
                    errors = [e for e in errors if not e.startswith("[%d]" % idx)]
            except UploadError as exc:
                _log(emit, "file.fail", index=idx, code=exc.code, message=str(exc)[:120])
            finally:
                scheduler.release()

    if errors or any(v is None for v in results):
        msg = "; ".join(errors) or "有文件未能完成上传"
        _log(emit, "batch.fail", sec=round(_now() - batch_began, 2), errors=len(errors))
        emit({"event": "fail", "message": msg})
        return 1

    entries = [v for v in results if v]
    entries.sort(key=lambda e: e["display_index"])
    if args.no_publish:
        total_sec = round(_now() - batch_began, 2)
        ok_bytes = sum(os.path.getsize(files[e["display_index"]]) for e in entries)
        _log(emit, "batch.done", files=len(entries), bytes=ok_bytes, sec=total_sec,
             published=False, concurrency=scheduler.effective_limit)
        emit({"event": "posted", "images": len(entries), "retcode": 0,
              "published": False, "urls": [e["picUrl"] for e in entries]})
        return 0
    emit({"event": "publishing", "images": len(entries)})
    t_pub = _now()
    result = publish_feed(cookie, bkn, entries)
    pub_sec = round(_now() - t_pub, 2)
    if result.get("retcode") != 0:
        _log(emit, "publish.fail", sec=pub_sec, code=result.get("retcode"))
        emit({"event": "fail", "message": "发布失败: %s %s" % (result.get("retcode"), result.get("message"))})
        return 1

    total_sec = round(_now() - batch_began, 2)
    ok_bytes = sum(os.path.getsize(files[e["display_index"]]) for e in entries)
    _log(emit, "batch.done", files=len(entries), bytes=ok_bytes, sec=total_sec,
         mib_per_s=round(ok_bytes / 1048576 / total_sec, 2) if total_sec else 0,
         publish_sec=pub_sec, concurrency=scheduler.effective_limit)
    emit({"event": "posted", "images": len(entries), "retcode": 0,
          "published": True, "urls": [e["picUrl"] for e in entries]})
    return 0


def cmd_stream(args, emit) -> int:
    """逐张接收临时载体；每张完成就回传直链，调用方随后可删除本地文件。"""
    cookie = load_cookies(args.session)
    bkn = _bkn(_cookie_value(cookie, "p_skey"))
    entries: list[dict] = []
    publish_batch: list[dict] = []
    urls: dict[int, str] = {}
    published_batches = 0

    def publish_pending() -> None:
        nonlocal published_batches
        if args.no_publish or not publish_batch:
            return
        emit({"event": "publishing", "batch": published_batches + 1,
              "images": len(publish_batch)})
        result = publish_feed(cookie, bkn, publish_batch)
        if result.get("retcode") != 0:
            raise RuntimeError("发布失败: %s %s" %
                               (result.get("retcode"), result.get("message") or ""))
        published_batches += 1
        emit({"event": "published_batch", "batch": published_batches,
              "images": len(publish_batch)})
        publish_batch.clear()

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            command = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"流式命令不是有效 JSON：{exc}") from exc

        action = command.get("cmd")
        if action == "upload":
            encoded_path = str(command.get("path_b64") or "")
            if encoded_path:
                encoded_path += "=" * (-len(encoded_path) % 4)
                try:
                    path = base64.urlsafe_b64decode(encoded_path).decode("utf-8")
                except (ValueError, UnicodeDecodeError) as exc:
                    raise RuntimeError("流式上传路径编码无效") from exc
            else:
                path = str(command.get("path") or "")
            index = int(command.get("index", len(entries)))
            if not path or not os.path.isfile(path):
                raise RuntimeError(f"待上传载体不存在：{path}")
            out = slice_upload(path, cookie, bkn, emit, index, max_retries=3)
            if not out["url"]:
                raise RuntimeError("分片全部成功但没拿到原图地址")
            width, height = png_size(path)
            task_id = str(random.randint(10 ** 16, 10 ** 17 - 1))
            entry = {"task_id": task_id, "picId": task_id, "picUrl": out["url"],
                     "width": width, "height": height, "display_index": index}
            entries.append(entry)
            publish_batch.append(entry)
            urls[index] = out["url"]
            emit({"event": "uploaded", "index": index, "url": out["url"],
                  "bytes": out["size"], "entry": entry})
            if len(publish_batch) >= MAX_IMAGES:
                publish_pending()
        elif action == "finish":
            publish_pending()
            ordered_urls = [urls[i] for i in sorted(urls)]
            emit({"event": "posted", "images": len(ordered_urls), "retcode": 0,
                  "published": not args.no_publish,
                  "published_batches": published_batches,
                  "urls": ordered_urls})
            return 0
        else:
            raise RuntimeError(f"未知流式命令：{action}")

    raise RuntimeError("流式上传输入提前结束，未收到 finish")



# ---------------------------------------------------------------- 加入频道


def guild_join_setting(cookie: str, bkn: int, guild_id: str = GUILD_ID) -> dict:
    st, resp = _post_keepalive(
        "/qunng/guild/gotrpc/auth/trpc.group_pro.cmd0x908e.JoinGuildSetting/HandleProcess?bkn=%d" % bkn,
        json.dumps({"uint64_guild_id": guild_id}).encode(), cookie,
        {"x-oidb": json.dumps({"uint32_service_type": "1"}),
         "content-type": "application/json"})
    return json.loads(resp) if resp else {"retcode": st}


def join_guild(cookie: str, bkn: int, guild_id: str = GUILD_ID) -> dict:
    """加入频道。实测只需 uint64_guild_id，不需要 x-turing-signature。"""
    st, resp = _post_keepalive(
        "/qunng/guild/gotrpc/auth/trpc.group_pro.cmd0xf67.AddGuildMember/HandleProcess1?bkn=%d" % bkn,
        json.dumps({"uint64_guild_id": guild_id}).encode(), cookie,
        {"x-oidb": json.dumps({"uint32_service_type": "1"}),
         "content-type": "application/json"})
    return json.loads(resp) if resp else {"retcode": st}


def my_guilds(cookie: str, bkn: int) -> dict:
    st, resp = _post_keepalive(
        "/qunng/guild/gotrpc/auth/trpc.group_pro.cmd0xf59.GetMyGuildInfo/HandleProcess3?bkn=%d" % bkn,
        b"{}", cookie, {"content-type": "application/json"})
    return json.loads(resp) if resp else {"retcode": st}


def cmd_join(args, emit) -> int:
    cookie = load_cookies(args.session)
    bkn = _bkn(_cookie_value(cookie, "p_skey"))
    guild = args.guild or GUILD_ID

    setting = guild_join_setting(cookie, bkn, guild)
    join_type = ((setting.get("data") or {}).get("setting") or {}).get("join_type")
    _log(emit, "join.setting", retcode=setting.get("retcode"), join_type=join_type)
    if join_type not in (None, 1):
        emit({"event": "fail", "message": "该频道加入方式为审批制（join_type=%s），需人工申请" % join_type})
        return 1

    result = join_guild(cookie, bkn, guild)
    retcode = result.get("retcode")
    data = result.get("data") or {}
    joined = data.get("joined_guilds_result") or []
    _log(emit, "join.result", retcode=retcode, joined=len(joined),
         has_channels=bool(data.get("guild_channels")))
    if retcode != 0:
        emit({"event": "fail", "message": "加入失败(%s): %s" % (retcode, result.get("message") or "")})
        return 1

    check = my_guilds(cookie, bkn)
    ids = {g.get("uint64_guild_id") for g in ((check.get("data") or {}).get("msg_rsp_change_guilds") or [])}
    inside = guild in ids
    emit({"event": "joined", "guild_id": guild, "verified": inside,
          "guilds": len(ids)})
    return 0 if inside else 3


def main() -> int:
    parser = argparse.ArgumentParser(description="藏笺 QQ 频道上传器")
    parser.add_argument("--json", action="store_true", help="输出 JSON 行")
    parser.add_argument("--session", default=SESSION_PATH, help="会话文件路径")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_up = sub.add_parser("upload", help="上传载体并返回直链")
    p_up.add_argument("--file", action="append", required=True, help="载体 PNG，可重复")
    p_up.set_defaults(func=cmd_upload)

    p_st = sub.add_parser("status", help="检查登录态")
    p_st.set_defaults(func=cmd_status)

    p_po = sub.add_parser("post", help="分片上传并发图片帖")
    p_po.add_argument("--file", action="append", required=True, help="图片，可重复（最多 18 张）")
    p_po.add_argument("--concurrency", type=int,
                      default=int(os.environ.get("CJ_FILE_CONCURRENCY", "1")),
                      help="同时上传的文件数（默认 1；实测 2 路优势不明确，见验收报告）")
    p_po.add_argument("--no-publish", action="store_true",
                      help="只上传并返回直链，不发布频道帖子")
    p_po.set_defaults(func=cmd_post)

    p_stream = sub.add_parser("stream", help="逐张上传临时载体并按 18 张自动拆帖")
    p_stream.add_argument("--no-publish", action="store_true",
                          help="只上传并返回直链，不发布频道帖子")
    p_stream.set_defaults(func=cmd_stream)

    p_join = sub.add_parser("join", help="加入频道")
    p_join.add_argument("--guild", default=None, help="频道 guild_id（默认当前频道）")
    p_join.set_defaults(func=cmd_join)

    p_ck = sub.add_parser("check", help="校验直链")
    p_ck.add_argument("--url", required=True)
    p_ck.set_defaults(func=cmd_check)

    args = parser.parse_args()

    def emit(event: dict) -> None:
        if args.json:
            print(json.dumps(event, ensure_ascii=False), flush=True)
        else:
            kind = event.get("event")
            if kind == "progress":
                print(f"[{event['index']}] {event['percent']}% (总 {event['overall']}%)", flush=True)
            else:
                print(json.dumps(event, ensure_ascii=False), flush=True)

    try:
        return args.func(args, emit)
    except Exception as exc:  # noqa: BLE001
        emit({"event": "fail", "message": str(exc)})
        return 2


if __name__ == "__main__":
    sys.exit(main())
