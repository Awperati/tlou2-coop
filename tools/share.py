"""Tiny LAN file drop for moving logs and saves between the two PCs. No install on the other PC:
open http://<this PC's LAN address>:8765 in a browser there to upload files, or download the files
already in the folder. Only answers LAN addresses (192.168.x, 10.x, 172.16-31.x).
  python -I tools/share.py [folder] [port]      (default folder: <repo>/share, port 8765)
"""
import html, ipaddress, os, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "share"))
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
os.makedirs(ROOT, exist_ok=True)

PAGE = """<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>TLOU2 Co-op file drop</title>
<style>body{font:16px system-ui;max-width:40em;margin:2em auto;padding:0 1em}li{margin:.3em 0}</style>
<h1>TLOU2 Co-op file drop</h1>
<p>Send files to this PC:</p>
<input type="file" id="f" multiple> <button onclick="up()">Upload</button> <span id="s"></span>
<h2>Files here</h2><ul>%s</ul>
<script>
async function up(){const s=document.getElementById('s');
 for(const f of document.getElementById('f').files){s.textContent='Sending '+f.name+'...';
  const r=await fetch('/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
  if(!r.ok){s.textContent='Failed: '+f.name;return}}
 s.textContent='Done.';setTimeout(()=>location.reload(),600)}
</script>"""


def lan(addr):
    ip = ipaddress.ip_address(addr)
    return ip.is_private or ip.is_loopback


class H(BaseHTTPRequestHandler):
    def deny(self):
        if lan(self.client_address[0]):
            return False
        self.send_error(403)
        return True

    def do_GET(self):
        if self.deny():
            return
        path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path).lstrip("/")
        if not path:
            items = "".join('<li><a href="/%s">%s</a> (%d KB)</li>' % (urllib.parse.quote(n), html.escape(n),
                            os.path.getsize(os.path.join(ROOT, n)) // 1024)
                            for n in sorted(os.listdir(ROOT)) if os.path.isfile(os.path.join(ROOT, n)))
            body = (PAGE % (items or "<li>(none yet)</li>")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        name = os.path.basename(path)
        full = os.path.join(ROOT, name)
        if not os.path.isfile(full):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.send_header("Content-Length", str(os.path.getsize(full)))
        # A real modified time, so a downloaded copy gets a fresh timestamp (the mod's loader
        # reloads coop.dll on a timestamp change).
        self.send_header("Last-Modified", self.date_time_string(int(os.path.getmtime(full))))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with open(full, "rb") as f:
            while chunk := f.read(1 << 20):
                self.wfile.write(chunk)

    def do_POST(self):
        if self.deny():
            return
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        name = os.path.basename(q.get("name", [""])[0]).strip()
        if not name or name.startswith("."):
            self.send_error(400)
            return
        left = int(self.headers.get("Content-Length", 0))
        with open(os.path.join(ROOT, name), "wb") as f:
            while left > 0:
                chunk = self.rfile.read(min(left, 1 << 20))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
        print("received", name, flush=True)
        self.send_response(200)
        self.end_headers()


print("Sharing %s on port %d. On the other PC, open http://<this PC's address>:%d" % (ROOT, PORT, PORT), flush=True)
ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
