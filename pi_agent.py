#!/usr/bin/env python3
"""SmallTV Pi Monitor agent — serves Raspberry Pi resource stats as JSON.

Zero dependencies (stdlib only). Reads real hardware values:
  cpu %, RAM used/total (GiB), disk used/total of / (GiB), CPU temp (C), hostname.

Run:            python3 pi_agent.py            # serves on 0.0.0.0:8080
Custom port:    PORT=9000 python3 pi_agent.py

Autostart (systemd) — /etc/systemd/system/pi-agent.service:
  [Unit]
  Description=SmallTV Pi Monitor agent
  After=network.target
  [Service]
  ExecStart=/usr/bin/python3 /home/pi/pi_agent.py
  Restart=always
  [Install]
  WantedBy=multi-user.target
  # then: sudo systemctl enable --now pi-agent
"""
import http.server, socketserver, json, os, time, socket

GIB = 1024 ** 3
DISK_PATH = os.environ.get("DISK_PATH", "/")


_cpu_prev = None
def cpu_percent():
    """CPU % averaged over the time since the previous call (top-style),
    instead of a fresh short window each time -> smoother, rarely a spurious 0."""
    global _cpu_prev
    def snap():
        with open("/proc/stat") as f:
            v = list(map(int, f.readline().split()[1:]))
        return v[3] + (v[4] if len(v) > 4 else 0), sum(v)   # idle+iowait, total
    cur = snap()
    if _cpu_prev is None:              # first call: take a short baseline
        time.sleep(0.3)
        prev, cur = cur, snap()
    else:
        prev = _cpu_prev
    _cpu_prev = cur
    di, dt = cur[0] - prev[0], cur[1] - prev[1]
    return round(100.0 * (1 - di / dt), 1) if dt > 0 else 0.0


def mem_gib():
    d = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, _, rest = line.partition(":")
            d[k] = int(rest.split()[0])          # KiB
    total = d["MemTotal"] / 1024 / 1024           # KiB -> GiB
    avail = d.get("MemAvailable", d.get("MemFree", 0)) / 1024 / 1024
    return round(total - avail, 2), round(total, 2)  # used, total (GiB, measured)


def disk_gib(path=DISK_PATH):
    s = os.statvfs(path)
    total = s.f_blocks * s.f_frsize
    free = s.f_bavail * s.f_frsize
    return round((total - free) / GIB, 1), round(total / GIB, 1)  # used, total


def cpu_temp():
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return round(int(f.read()) / 1000.0, 1)
    except OSError:
        return None


def stats():
    mu, mt = mem_gib()
    du, dt = disk_gib()
    return {
        "host": socket.gethostname(),
        "cpu": cpu_percent(),
        "mem_used": mu, "mem_total": mt,
        "disk_used": du, "disk_total": dt,
        "temp": cpu_temp(),
        "uptime": int(float(open("/proc/uptime").read().split()[0])),
    }


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps(stats()).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))  # ESP-friendly (no chunked)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):   # stay quiet
        pass


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8080"))
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", port), Handler) as srv:
        print(f"pi-agent serving stats on :{port}  (disk={DISK_PATH})")
        srv.serve_forever()
