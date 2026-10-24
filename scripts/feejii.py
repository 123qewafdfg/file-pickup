#!/usr/bin/env python3
"""小飞机网盘直连客户端：二维码登录、上传与永久分享，全程不启动浏览器。"""

from __future__ import annotations
import argparse, base64, ctypes, ctypes.wintypes as wt, hashlib, hmac, json
import os, secrets, socket, ssl, struct, sys, time, uuid as uuidlib
import urllib.error, urllib.parse, urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path

API_BASE = "https://api.feijipan.com"
WEB_BASE = "https://www.feejii.com"
WS_HOST = "api.feejii.com"
AES_KEY = b"dingHao-disk-app"
COS_HOST = "1500033322.vodpro-upload.com"
COS_BASE = "https://" + COS_HOST
COS_BUCKET_ID = "9b5hmmeuqkg00qf"
CHUNK_SIZE = 4 * 1024 * 1024
SIMPLE_LIMIT = 8 * 1024 * 1024
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131 Safari/537.36"


def emit(event: str, **fields) -> None:
    print(json.dumps({"event": event, **fields}, ensure_ascii=False), flush=True)


def session_path() -> Path:
    root = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    return Path(root) / "FilePickup" / "feejii-session.dat"


class DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_ubyte))]


def _blob(data: bytes):
    buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    return DATA_BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte))), buf


def _dpapi(data: bytes, protect: bool) -> bytes:
    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    source, keepalive = _blob(data)
    result = DATA_BLOB()
    if protect:
        ok = crypt32.CryptProtectData(
            ctypes.byref(source), "FilePickup Feijipan", None, None, None, 1, ctypes.byref(result)
        )
    else:
        ok = crypt32.CryptUnprotectData(
            ctypes.byref(source), None, None, None, None, 1, ctypes.byref(result)
        )
    if not ok:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return ctypes.string_at(result.pbData, result.cbData)
    finally:
        kernel32.LocalFree(result.pbData)


def save_session(session: dict) -> None:
    path = session_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    raw = json.dumps(session, ensure_ascii=False, separators=(",", ":")).encode()
    temp.write_bytes(_dpapi(raw, True))
    os.replace(temp, path)


def load_session() -> dict | None:
    path = session_path()
    if not path.exists():
        return None
    try:
        value = json.loads(_dpapi(path.read_bytes(), False).decode())
        return value if value.get("appToken") and value.get("uuid") else None
    except Exception:
        return None


def clear_session() -> None:
    try:
        session_path().unlink(missing_ok=True)
    except OSError:
        pass


def aes_encrypt(text: str) -> str:
    """Windows CNG：AES-128-ECB + PKCS7，与网站实现一致。"""
    bcrypt = ctypes.WinDLL("bcrypt", use_last_error=True)
    alg, key = ctypes.c_void_p(), ctypes.c_void_p()
    status = bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0)
    if status:
        raise OSError(f"打开 AES 失败：0x{status & 0xffffffff:08x}")
    try:
        mode = "ChainingModeECB".encode("utf-16-le") + b"\0\0"
        mode_buf = (ctypes.c_ubyte * len(mode)).from_buffer_copy(mode)
        status = bcrypt.BCryptSetProperty(alg, "ChainingMode", mode_buf, len(mode), 0)
        if status:
            raise OSError(f"设置 AES 模式失败：0x{status & 0xffffffff:08x}")
        obj_size, got = wt.DWORD(), wt.DWORD()
        status = bcrypt.BCryptGetProperty(
            alg, "ObjectLength", ctypes.byref(obj_size), ctypes.sizeof(obj_size), ctypes.byref(got), 0
        )
        if status:
            raise OSError(f"读取 AES 属性失败：0x{status & 0xffffffff:08x}")
        key_obj = (ctypes.c_ubyte * obj_size.value)()
        secret = (ctypes.c_ubyte * len(AES_KEY)).from_buffer_copy(AES_KEY)
        status = bcrypt.BCryptGenerateSymmetricKey(
            alg, ctypes.byref(key), key_obj, obj_size.value, secret, len(AES_KEY), 0
        )
        if status:
            raise OSError(f"生成 AES 密钥失败：0x{status & 0xffffffff:08x}")
        raw = text.encode()
        raw_buf = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
        out_size = wt.DWORD()
        status = bcrypt.BCryptEncrypt(
            key, raw_buf, len(raw), None, None, 0, None, 0, ctypes.byref(out_size), 1
        )
        if status:
            raise OSError(f"计算 AES 长度失败：0x{status & 0xffffffff:08x}")
        output = (ctypes.c_ubyte * out_size.value)()
        status = bcrypt.BCryptEncrypt(
            key, raw_buf, len(raw), None, None, 0, output, out_size.value, ctypes.byref(got), 1
        )
        if status:
            raise OSError(f"AES 加密失败：0x{status & 0xffffffff:08x}")
        return bytes(output[: got.value]).hex()
    finally:
        if key:
            bcrypt.BCryptDestroyKey(key)
        if alg:
            bcrypt.BCryptCloseAlgorithmProvider(alg, 0)


def api_params(client_uuid: str, token: str | None) -> dict:
    result = {
        "uuid": client_uuid, "devType": "6", "devCode": client_uuid,
        "devModel": "chrome", "devVersion": "131", "appVersion": "",
        "timestamp": aes_encrypt(str(int(time.time() * 1000))), "extra": 2,
    }
    if token:
        result["appToken"] = token
    return result


def http_json(method: str, url: str, *, params=None, body=None, timeout=30) -> dict:
    if params:
        url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
    data = None
    headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
    if body is not None:
        data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
        headers["Content-Type"] = "application/json;charset=UTF-8"
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:500]
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
    try:
        return json.loads(raw.decode()) if raw else {}
    except Exception as exc:
        raise RuntimeError("服务器返回了无法解析的数据") from exc


def api_request(method: str, path: str, session: dict, body=None, extra=None, timeout=30) -> dict:
    params = api_params(session["uuid"], session.get("appToken"))
    if extra:
        params.update(extra)
    payload = http_json(method, API_BASE + path, params=params, body=body, timeout=timeout)
    if str(payload.get("code", 200)) != "200":
        raise RuntimeError(str(payload.get("msg") or payload.get("message") or "接口请求失败"))
    return payload


def deep_value(value, name: str):
    queue, checked = [value], 0
    while queue and checked < 120:
        current = queue.pop(0)
        checked += 1
        if isinstance(current, dict):
            if name in current and current[name] not in (None, ""):
                return current[name]
            queue.extend(current.values())
        elif isinstance(current, list):
            queue.extend(current)
    return None


class WebSocket:
    def __init__(self, host: str, path: str, timeout: float):
        raw = socket.create_connection((host, 443), timeout=timeout)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
        self.sock.settimeout(timeout)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            f"Origin: {WEB_BASE}\r\nUser-Agent: {USER_AGENT}\r\n\r\n"
        ).encode("ascii")
        self.sock.sendall(request)
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("WebSocket 握手中断")
            response.extend(chunk)
        header, self.buffer = bytes(response).split(b"\r\n\r\n", 1)
        if b" 101 " not in header.split(b"\r\n", 1)[0]:
            raise RuntimeError("WebSocket 握手失败")
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        )
        accept = next(
            (line.split(b":", 1)[1].strip() for line in header.split(b"\r\n")
             if line.lower().startswith(b"sec-websocket-accept:")), None
        )
        if accept != expected:
            raise RuntimeError("WebSocket 服务身份校验失败")

    def close(self):
        try:
            self.send_frame(8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def read_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(4096, count - len(self.buffer)))
            if not chunk:
                raise EOFError("WebSocket 已关闭")
            self.buffer += chunk
        value, self.buffer = self.buffer[:count], self.buffer[count:]
        return value

    def send_frame(self, opcode: int, payload: bytes):
        mask = secrets.token_bytes(4)
        length = len(payload)
        if length < 126:
            header = bytes((0x80 | opcode, 0x80 | length))
        elif length < 65536:
            header = bytes((0x80 | opcode, 0xFE)) + struct.pack("!H", length)
        else:
            header = bytes((0x80 | opcode, 0xFF)) + struct.pack("!Q", length)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_text(self, text: str):
        self.send_frame(1, text.encode())

    def receive_text(self) -> str | None:
        parts = []
        while True:
            first, second = self.read_exact(2)
            final, opcode, length = bool(first & 0x80), first & 0x0F, second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self.read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.read_exact(8))[0]
            mask = self.read_exact(4) if second & 0x80 else b""
            payload = self.read_exact(length)
            if mask:
                payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
            if opcode == 8:
                return None
            if opcode == 9:
                self.send_frame(10, payload)
                continue
            if opcode in (0, 1):
                parts.append(payload)
                if final:
                    return b"".join(parts).decode("utf-8", "replace")


def login(qr_output: str, timeout: int) -> int:
    client_uuid = str(uuidlib.uuid4())
    now_ms = str(int(time.time() * 1000))
    qr_text = f"{WEB_BASE}/ws/qr/{aes_encrypt(client_uuid + '-' + now_ms)}"
    qr_path = Path(qr_output)
    qr_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        import segno
        segno.make(qr_text).save(str(qr_path), scale=7, border=2)
    except Exception as exc:
        emit("fail", message=f"生成二维码失败：{exc}")
        return 1
    emit("qr", png=str(qr_path), url=qr_text)
    ws, deadline, last_heartbeat = None, time.monotonic() + timeout, 0.0
    try:
        ws = WebSocket(WS_HOST, f"/ws/webSocket/{client_uuid}-{now_ms}", min(15, timeout))
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now - last_heartbeat >= 25:
                ws.send_text("heartbeat")
                last_heartbeat = now
            ws.sock.settimeout(min(5.0, max(0.2, deadline - now)))
            try:
                message = ws.receive_text()
            except (socket.timeout, TimeoutError, ssl.SSLError) as exc:
                if isinstance(exc, ssl.SSLError) and "timed out" not in str(exc).lower():
                    raise
                continue
            if message is None:
                raise RuntimeError("登录连接已关闭")
            if message == "heartbeat":
                continue
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                continue
            code = str(data.get("code"))
            if code == "1":
                emit("scanned")
            elif code == "-1":
                emit("expired", message="二维码已过期")
                return 2
            elif code == "200":
                token = deep_value(data, "token")
                if not token:
                    raise RuntimeError("登录成功响应中缺少令牌")
                save_session({"appToken": str(token), "uuid": client_uuid, "savedAt": int(time.time())})
                emit("logged_in")
                return 0
        emit("timeout", message="扫码登录超时")
        return 2
    except Exception as exc:
        emit("fail", message=f"小飞机直连登录失败：{exc}")
        return 1
    finally:
        if ws:
            ws.close()


def file_md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def cos_quote(value) -> str:
    return urllib.parse.quote(str(value), safe="-_.~")


def cos_authorization(method: str, object_path: str, query: dict, headers: dict, cred: dict) -> str:
    now = int(time.time())
    try:
        expiry = int(float(cred.get("expiration", now + 3600)))
    except (TypeError, ValueError):
        expiry = now + 3600
    key_time = f"{max(0, now - 60)};{max(now + 60, min(expiry, now + 3600))}"
    sign_key = hmac.new(str(cred["secretAccessKey"]).encode(), key_time.encode(), hashlib.sha1).hexdigest()
    hvals = {str(k).lower(): " ".join(str(v).strip().split()) for k, v in headers.items()}
    qvals = {str(k).lower(): str(v) for k, v in query.items()}
    hnames, qnames = sorted(hvals), sorted(qvals)
    canonical_headers = "&".join(f"{cos_quote(k)}={cos_quote(hvals[k])}" for k in hnames)
    canonical_query = "&".join(f"{cos_quote(k)}={cos_quote(qvals[k])}" for k in qnames)
    uri = "/" + "/".join(cos_quote(p) for p in object_path.strip("/").split("/"))
    http_string = f"{method.lower()}\n{uri}\n{canonical_query}\n{canonical_headers}\n"
    string_to_sign = f"sha1\n{key_time}\n{hashlib.sha1(http_string.encode()).hexdigest()}\n"
    signature = hmac.new(sign_key.encode(), string_to_sign.encode(), hashlib.sha1).hexdigest()
    return (
        f"q-sign-algorithm=sha1&q-ak={cos_quote(cred['accessKeyId'])}&q-sign-time={key_time}"
        f"&q-key-time={key_time}&q-header-list={cos_quote(';'.join(hnames))}"
        f"&q-url-param-list={cos_quote(';'.join(qnames))}&q-signature={signature}"
    )


def cos_request(method: str, object_path: str, cred: dict, *, query=None, headers=None, data=b"", timeout=120):
    query = query or {}
    # 官网 subAppBucket 模式会先把 bucketId 拼入 Key：
    # 9b5hmmeuqkg00qf/storage/files/...。该完整 Key 必须同时用于 URL 与签名。
    signed_path = f"{COS_BUCKET_ID}/{object_path.lstrip('/')}"
    signed = {"host": COS_HOST}
    if cred.get("sessionToken"):
        signed["x-cos-security-token"] = str(cred["sessionToken"])
    if headers:
        signed.update({str(k).lower(): str(v) for k, v in headers.items()})
    authorization = cos_authorization(method, signed_path, query, signed, cred)
    url = COS_BASE + "/" + "/".join(cos_quote(p) for p in signed_path.strip("/").split("/"))
    if query:
        url += "?" + urllib.parse.urlencode(query)
    request_headers = {k: v for k, v in signed.items() if k != "host"}
    request_headers.update({"Authorization": authorization, "Host": COS_HOST})
    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.headers, response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:1000]
        raise RuntimeError(f"对象存储 HTTP {exc.code}: {detail}") from exc


def upload_to_cos(path: Path, object_path: str, cred: dict, callback: str) -> None:
    total = path.stat().st_size
    callback_headers = {"x-amz-meta-callback": callback}
    if total <= SIMPLE_LIMIT:
        emit("upload_progress", file=str(path), percent=1)
        cos_request("PUT", object_path, cred, headers=callback_headers, data=path.read_bytes(), timeout=300)
        emit("upload_progress", file=str(path), percent=100)
        return
    _, _, raw = cos_request(
        "POST", object_path, cred, query={"uploads": ""}, headers=callback_headers, timeout=60
    )
    try:
        root = ET.fromstring(raw)
        upload_id = next(node.text for node in root.iter() if node.tag.endswith("UploadId"))
    except Exception as exc:
        raise RuntimeError("对象存储没有返回分片上传编号") from exc
    parts, sent = [], 0
    with path.open("rb") as stream:
        number = 1
        while True:
            block = stream.read(CHUNK_SIZE)
            if not block:
                break
            _, response_headers, _ = cos_request(
                "PUT", object_path, cred,
                query={"partNumber": str(number), "uploadId": upload_id},
                data=block, timeout=300,
            )
            etag = response_headers.get("ETag")
            if not etag:
                raise RuntimeError(f"第 {number} 个分片缺少 ETag")
            parts.append((number, etag))
            sent += len(block)
            emit("upload_progress", file=str(path), percent=min(99, int(sent * 100 / max(1, total))))
            number += 1
    xml = ["<CompleteMultipartUpload>"]
    for number, etag in parts:
        xml.append(f"<Part><PartNumber>{number}</PartNumber><ETag>{etag}</ETag></Part>")
    xml.append("</CompleteMultipartUpload>")
    cos_request(
        "POST", object_path, cred, query={"uploadId": upload_id},
        headers={"content-type": "application/xml"},
        data="".join(xml).encode(), timeout=120,
    )
    emit("upload_progress", file=str(path), percent=100)


def user_info(session: dict) -> tuple[str, str]:
    payload = api_request("GET", "/app/user/info/map", session)
    user_id = deep_value(payload, "userId") or deep_value(payload, "id")
    account = deep_value(payload, "account") or deep_value(payload, "mobile")
    if not user_id or not account:
        raise RuntimeError("无法读取小飞机账号信息，请重新登录")
    return str(user_id), str(account)


def upload_one(path: Path, session: dict, user_id: str, account: str) -> str:
    if not path.is_file():
        raise RuntimeError(f"文件不存在：{path}")
    emit("upload_preparing", file=str(path), name=path.name)
    digest, size = file_md5(path), path.stat().st_size
    size_kib = max(1, round(size / 1024))
    now, stamp = datetime.now(), str(int(time.time() * 1000))
    object_path = f"storage/files/{now:%Y/%m/%d}/{account[-1:] or '0'}/{account}/{stamp}.gz"
    token_body = {
        "fileName": path.name, "fileType": 1, "userId": user_id, "md5": digest,
        "pathKey": object_path, "fileSize": str(size_kib), "folderId": 0, "fileId": "",
    }
    response = api_request("POST", "/app/vod/getUpToken", session, body=token_body, timeout=60)
    up_token, file_id = deep_value(response, "upToken"), deep_value(response, "fileId")
    if str(up_token) != "-1":
        cred = {
            "accessKeyId": deep_value(response, "accessKeyId"),
            "secretAccessKey": deep_value(response, "secretAccessKey"),
            "sessionToken": deep_value(response, "sessionToken"),
            "expiration": deep_value(response, "expiration"),
        }
        if not cred["accessKeyId"] or not cred["secretAccessKey"]:
            raise RuntimeError("服务器没有返回上传凭证")
        callback_body = dict(token_body)
        callback_body["fileSize"] = size_kib * 1024
        callback_json = {
            "callbackMode": "HTTP_ASYNC",
            "callbackBody": json.dumps(callback_body, ensure_ascii=False, separators=(",", ":")),
        }
        callback = base64.b64encode(
            json.dumps(callback_json, ensure_ascii=False, separators=(",", ":")).encode()
        ).decode()
        upload_to_cos(path, object_path, cred, callback)
        deadline = time.monotonic() + max(120, min(1800, int(size / 1048576) * 4 + 120))
        while time.monotonic() < deadline:
            result = api_request(
                "GET", "/ws/vod/results", session, extra={"token": object_path}, timeout=30
            )
            status, current_id = deep_value(result, "status"), deep_value(result, "fileId")
            if str(status) == "1" and current_id:
                file_id = current_id
                break
            if str(status) in ("2", "-2"):
                raise RuntimeError(str(deep_value(result, "msg") or "服务器处理上传文件失败"))
            time.sleep(2)
        else:
            raise RuntimeError("上传完成，但服务器处理超时")
    if not file_id:
        raise RuntimeError("上传完成响应中缺少文件编号")
    emit("upload_complete", file=str(path), fileId=str(file_id))
    emit("share_creating", file=str(path))
    share = api_request("POST", "/app/share/url", session, body={
        "shareId": "", "fileIds": str(file_id), "folderIds": "", "code": "",
        "amt": "", "term": 0, "showRecommend": 0, "showUpTime": 0,
        "showDownloads": 0, "showComments": 0, "showStars": 0, "showLikes": 0,
    }, timeout=60)
    url = deep_value(share, "shareUrl") or deep_value(share, "url")
    if not url:
        raise RuntimeError("永久分享创建成功，但服务器没有返回链接")
    if str(url).startswith("/"):
        url = WEB_BASE + str(url)
    emit("share_complete", file=str(path), url=str(url), term=0)
    return str(url)


def status_command() -> int:
    session = load_session()
    if not session:
        emit("status", loggedIn=False)
        return 0
    try:
        user_id, account = user_info(session)
        emit("status", loggedIn=True, userId=user_id, account=account)
    except Exception as exc:
        clear_session()
        emit("status", loggedIn=False, message=str(exc))
    return 0


def logout_command() -> int:
    session = load_session()
    if session:
        try:
            api_request("POST", "/app/logout", session, body={})
        except Exception:
            pass
    clear_session()
    emit("logged_out")
    return 0


def upload_command(paths: list[str]) -> int:
    session = load_session()
    if not session:
        emit("upload_failed", message="小飞机尚未登录，请先扫码登录")
        return 1
    try:
        user_id, account = user_info(session)
        for raw in paths:
            upload_one(Path(raw).resolve(), session, user_id, account)
        return 0
    except Exception as exc:
        emit("upload_failed", message=f"小飞机直传失败：{exc}")
        return 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="小飞机网盘直连上传")
    parser.add_argument("--json", action="store_true")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--status", action="store_true")
    group.add_argument("--logout", action="store_true")
    group.add_argument("--upload", action="append", default=[])
    parser.add_argument("--qr-output", default=str(Path.cwd() / "feijii-login.png"))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--upload-timeout", type=int, default=1800)
    parser.add_argument("--port", type=int, default=0, help="兼容旧参数，不再使用浏览器")
    parser.add_argument("--profile", default="", help="兼容旧参数，不再使用浏览器")
    args = parser.parse_args(argv)
    if args.status:
        return status_command()
    if args.logout:
        return logout_command()
    if args.upload:
        return upload_command(args.upload)
    return login(args.qr_output, max(3, args.timeout))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        emit("fail", message="操作已取消")
        raise SystemExit(130)
    except Exception as exc:
        emit("fail", message=f"小飞机直连异常：{exc}")
        raise SystemExit(1)
