# -*- coding: utf-8 -*-
"""CDP 抓包：挂着 Edge，把分片上传/发帖请求的 body 全部抓下来。"""
import base64, json, os, socket, struct, subprocess, sys, time, urllib.request

PROFILE = r"E:\pz\qq-channel-edge-profile"
OUT = r"E:\pz\caches\edge-netlog\bodies.txt"
PORT = 9333
KEYS = ("ApplySliceUpload", "UploadSliceData", "PublishFeed", "GetTasks",
        "AddTasks", "CommWriter", "commwriter", "richmedia", "RichMedia",
        "AddGuildMember", "JoinGuildSetting", "GetMyGuildInfo", "VisitorPermit",
        "join", "Join", "guild_join")


def log(line):
    with open(OUT, "a", encoding="utf-8") as fh:
        fh.write(line + "\n")


class WS:
    def __init__(self, url):
        rest = url[len("ws://"):]
        hostport, path = rest.split("/", 1)
        host, port = hostport.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake closed")
            buf += chunk
        head, self.buf = buf.split(b"\r\n\r\n", 1)
        if b"101" not in head.split(b"\r\n")[0]:
            raise ConnectionError("handshake failed: " + head[:120].decode("latin1"))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _frame(self, opcode, payload):
        header = bytearray([0x80 | opcode])
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return bytes(header) + masked

    def send(self, obj):
        self.sock.sendall(self._frame(0x1, json.dumps(obj).encode()))

    def recv(self):
        chunks = b""
        while True:
            b1, b2 = self._read(2)
            opcode = b1 & 0x0F
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            data = self._read(length) if length else b""
            if opcode == 0x9:
                self.sock.sendall(self._frame(0xA, data))
                continue
            if opcode == 0x8:
                raise ConnectionError("close frame")
            chunks += data
            if b1 & 0x80:
                return chunks.decode("utf-8", "replace")


def launch_edge():
    for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"):
        if os.path.exists(p):
            subprocess.Popen([p, f"--remote-debugging-port={PORT}",
                              f"--user-data-dir={PROFILE}", "--no-first-run",
                              "--no-default-browser-check",
                              "https://pd.qq.com/g/pd55235600"])
            return True
    return False


def wait_port(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/version", timeout=3) as r:
                return json.load(r)
        except Exception:
            time.sleep(1)
    return None


def pages():
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=5) as r:
        return json.load(r)


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("")
    if not wait_port(5):
        log("launching edge...")
        if not launch_edge():
            log("NO EDGE FOUND")
            return
        if not wait_port(90):
            log("edge devtools port not ready")
            return
    log("devtools ready")
    target = None
    for _ in range(60):
        try:
            for p in pages():
                if p.get("type") == "page" and "pd.qq.com" in (p.get("url") or ""):
                    target = p
                    break
        except Exception:
            pass
        if target:
            break
        time.sleep(1)
    if not target:
        log("no page target")
        return
    log("target: " + target["url"][:120])
    ws = WS(target["webSocketDebuggerUrl"])
    msg_id = 0
    pending = {}
    resp_watch = {}

    def call(method, params=None):
        nonlocal msg_id
        msg_id += 1
        ws.send({"id": msg_id, "method": method, "params": params or {}})
        return msg_id

    call("Network.enable")
    call("Page.enable")
    log("READY — 现在去页面里发一条带图帖")

    while True:
        try:
            msg = json.loads(ws.recv())
        except ConnectionError as exc:
            log("ws closed: %s" % exc)
            return
        if "id" in msg:
            info = pending.pop(msg["id"], None)
            if info and "postData" in info:
                body = (msg.get("result") or {}).get("postData")
                if body:
                    log("")
                    log("### BODY " + info["url"][:150])
                    log(body[:4000])
            elif info and "body" in info:
                text = (msg.get("result") or {}).get("body")
                if text:
                    log("")
                    log("### RESPONSE " + info["url"][:150])
                    log(str(text)[:12000])
            continue
        method = msg.get("method")
        params = msg.get("params") or {}
        if method == "Network.loadingFinished":
            rid = params.get("requestId")
            if rid in resp_watch:
                pending[call("Network.getResponseBody", {"requestId": rid})] = {
                    "url": resp_watch.pop(rid), "body": True}
            continue
        if method == "Network.requestWillBeSent":
            url = params.get("request", {}).get("url", "")
            if "pd.qq.com" in url and params.get("request", {}).get("method") == "POST":
                log("SEQ " + url[:160])
            if any(k in url for k in KEYS):
                rid = params.get("requestId")
                log("")
                log(">>> " + params.get("request", {}).get("method", "?") + " " + url[:180])
                for k, v in (params.get("request", {}).get("headers") or {}).items():
                    kl = k.lower()
                    if kl in ("x-richmedia-ukey", "x-richmedia-range-end", "x-richmedia-cumulatesha1",
                              "appid", "business_type", "agent_type", "content-type", "x-oidb",
                              "x-turing-signature", "origin_url"):
                        log(f"    {k}: {str(v)[:200]}")
                pending[call("Network.getRequestPostData", {"requestId": rid})] = {"url": url}
                resp_watch[rid] = url


if __name__ == "__main__":
    main()
