#!/usr/bin/env python3
"""Read-only localhost viewer of an integrated display's LVGL flush pixels."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import struct
import threading
import urllib.request
import zlib

from run_ota_update import OPENER, resolve_token, validate_base_url

WIDTH, HEIGHT = 240, 280
FRAME_BYTES = WIDTH * HEIGHT * 2


def rgb565_png(frame, width=WIDTH, height=HEIGHT):
    if (width, height) not in {(240, 280), (480, 480)} or len(frame) != width * height * 2:
        raise ValueError('invalid frame size')
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            offset = (y * width + x) * 2
            color = frame[offset] | (frame[offset + 1] << 8)
            red, green, blue = color >> 11, (color >> 5) & 63, color & 31
            rows.extend(((red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2)))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


PAGE = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>LamaBuddy · 设备 3 在线画面</title><style>
:root{color-scheme:dark;font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#10130f;color:#eeeada}
body{margin:0;min-height:100vh;display:grid;place-content:center}main{display:flex;align-items:center;gap:64px;padding:40px}
article{width:280px}small{color:#adbaa9;letter-spacing:.1em}h1{font-size:30px;font-weight:500;line-height:1.4;margin:18px 0}
p{color:#b4b7aa;font-size:14px;line-height:1.8}figure{margin:0;padding:18px;background:#080a08;border:1px solid #343930;border-radius:36px;box-shadow:0 24px 60px #0006}
img{display:block;width:480px;height:560px;border-radius:28px;background:#0f130f}button{font:inherit;color:#eeeada;background:#272d24;border:1px solid #47503f;padding:10px 18px;border-radius:10px;cursor:pointer}
output{display:block;color:#e4bb77;font-size:13px;line-height:1.7;min-height:56px;margin:22px 0}a{color:#b1d0b6}
@media(max-width:850px){main{flex-direction:column;gap:24px;padding:24px}article{width:min(85vw,480px)}img{width:min(80vw,480px);height:auto;aspect-ratio:6/7}}
</style><main><article><small>LAMABUDDY / LIVE DEVICE</small><h1>设备 3<br>在线画面</h1>
<p>来自 ESP32 正在送往屏幕的像素。<br>240 × 280 · 局域网只读预览</p><output id="status">正在连接设备…</output>
<button id="pause">暂停预览</button><p>在实物上切换页面，这里会同步更新。<br>此窗口不发送咖啡机控制命令。</p></article>
<figure><img id="screen" alt="ESP32 真实渲染画面"></figure></main><script>
let paused=false,url=null,frames=0;const status=document.querySelector('#status'),screen=document.querySelector('#screen');
document.querySelector('#pause').onclick=()=>{paused=!paused;document.querySelector('#pause').textContent=paused?'继续预览':'暂停预览'};
async function update(){if(!paused){try{const response=await fetch('/frame.png',{cache:'no-store'});if(!response.ok)throw Error();
const next=URL.createObjectURL(await response.blob());screen.src=next;if(url)URL.revokeObjectURL(url);url=next;
status.textContent='已连接 · '+new Date().toLocaleTimeString()+' · 第 '+(++frames)+' 帧';}catch{status.textContent='设备暂未连接或画面未就绪，正在重试…'}}setTimeout(update,750)}update();
</script></html>'''.encode()


def make_handler(base_url, token, width=WIDTH, height=HEIGHT):
    if (width, height) not in {(240, 280), (480, 480)}:
        raise ValueError('unsupported display size')
    frame_bytes = width * height * 2
    page = PAGE
    if (width, height) == (480, 480):
        page = (PAGE.replace('设备 3'.encode(), b'2.16 AMOLED')
                .replace('240 × 280'.encode(), '480 × 480'.encode())
                .replace(b'height:560px', b'height:480px')
                .replace(b'aspect-ratio:6/7', b'aspect-ratio:1/1'))
    frame_lock = threading.Lock()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, code, mime, body):
            self.send_response(code)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
            self.send_header('Content-Security-Policy', "default-src 'self'; img-src 'self' blob:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            port = self.server.server_port
            if self.headers.get('Host') not in {f'127.0.0.1:{port}', f'localhost:{port}'}:
                return self.send(403, 'text/plain', b'Invalid local host')
            if self.path == '/':
                return self.send(200, 'text/html; charset=utf-8', page)
            if self.path != '/frame.png':
                return self.send(404, 'text/plain', b'Not found')
            if not frame_lock.acquire(blocking=False):
                return self.send(503, 'text/plain', b'Frame request in progress')
            try:
                request = urllib.request.Request(base_url + '/diagnostics/display/frame', headers={'Authorization': 'Bearer ' + token})
                with OPENER.open(request, timeout=4) as response:
                    if response.headers.get('X-Display-Format') != 'RGB565LE':
                        raise ValueError('invalid frame format')
                    if response.headers.get('X-Display-Width', str(WIDTH)) != str(width) or response.headers.get('X-Display-Height', str(HEIGHT)) != str(height):
                        raise ValueError('display profile mismatch')
                    frame = response.read(frame_bytes + 1)
                self.send(200, 'image/png', rgb565_png(frame, width, height))
            except Exception:
                self.send(502, 'text/plain', b'Device frame unavailable')
            finally:
                frame_lock.release()

        def do_POST(self):
            self.send(405, 'text/plain', b'Read-only preview')
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--token-line', type=int, default=1)
    parser.add_argument('--port', type=int, default=8799)
    args = parser.parse_args()
    args.token = None
    base_url = validate_base_url(args.base_url)
    token = resolve_token(args)
    request = urllib.request.Request(base_url + '/health')
    with OPENER.open(request, timeout=6) as response:
        health = json.load(response)
    dimensions = {'lamabuddy-esp32s3-touch': (240, 280), 'lamabuddy-esp32s3-amoled216': (480, 480)}.get(health.get('deviceModel'))
    if dimensions is None:
        parser.error('Target is not the integrated touchscreen profile')
    server = ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(base_url, token, *dimensions))
    print(f'Read-only device preview: http://127.0.0.1:{server.server_port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
