#!/usr/bin/env python3
"""Check pixel fidelity and that the local viewer cannot forward controls."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import struct
import threading
import urllib.error
import urllib.request
import zlib
from display_preview import FRAME_BYTES, WIDTH, HEIGHT, make_handler, rgb565_png


def main():
    frame = b'\x00\xf8\xe0\x07\x1f\x00' + bytes(FRAME_BYTES - 6)
    png = rgb565_png(frame)
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', png[16:24]) == (WIDTH, HEIGHT)
    offset = 8
    while png[offset+4:offset+8] != b'IDAT':
        offset += struct.unpack('>I', png[offset:offset+4])[0] + 12
    length = struct.unpack('>I', png[offset:offset+4])[0]
    rows = zlib.decompress(png[offset+8:offset+8+length])
    assert len(rows) == HEIGHT * (WIDTH * 3 + 1)
    assert rows[:10] == b'\0\xff\0\0\0\xff\0\0\0\xff'
    for bad in [b'', frame[:-1], frame+b'\0']:
        try: rgb565_png(bad)
        except ValueError: pass
        else: raise AssertionError('invalid size accepted')

    class Device(BaseHTTPRequestHandler):
        calls = 0
        def log_message(self, *args): pass
        def do_GET(self):
            assert self.path == '/diagnostics/display/frame'
            assert self.headers['Authorization'] == 'Bearer synthetic'
            Device.calls += 1
            self.send_response(200);self.send_header('X-Display-Format','RGB565LE');self.end_headers();self.wfile.write(frame)
    device = ThreadingHTTPServer(('127.0.0.1',0),Device)
    viewer = ThreadingHTTPServer(('127.0.0.1',0),make_handler(f'http://127.0.0.1:{device.server_port}','synthetic'))
    for server in [device,viewer]: threading.Thread(target=server.serve_forever,daemon=True).start()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    base = f'http://127.0.0.1:{viewer.server_port}'
    try:
        with opener.open(base+'/') as r: assert b'synthetic' not in r.read()
        with opener.open(base+'/frame.png') as r: assert r.read()==png and r.headers['Cache-Control']=='no-store'
        for path,headers,data,expected in [('/controls/power',{},b'{}',405),('/diagnostics/restart',{},None,404),('/',{'Host':'untrusted.example'},None,403)]:
            try: opener.open(urllib.request.Request(base+path,headers=headers,data=data))
            except urllib.error.HTTPError as error: assert error.code==expected
            else: raise AssertionError('write/invalid host accepted')
        assert Device.calls==1
    finally:
        for server in [viewer,device]: server.shutdown();server.server_close()
    print('Display preview PASS: RGB565 fidelity, bounded frames, token isolation and read-only forwarding')

if __name__=='__main__': main()
