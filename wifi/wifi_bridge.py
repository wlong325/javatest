# -*- coding: utf-8 -*-
"""
WiFi TCP -> HTTP 桥：ESP-01S 板子 -> Spring Boot 后端
用法：
    pip install requests
    python wifi_bridge.py [后端地址]
    例：python wifi_bridge.py http://localhost:8080

板子数据格式：
    S,<温度×100>,<光敏mV>,<电位器mV>,<频率Hz>\r\n
    例：S,2706,2647,5000,142   （2706=27.06℃，2647mV=2.647V）

下行命令（后端 -> 桥 -> 板子）：
    R1/R0 继电器开/关，B1/B0 蜂鸣器开/关
"""
import socket
import threading
import requests
import re
import sys
import time

HOST = "0.0.0.0"
PORT = 8888
BASE = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080"
API = BASE + "/api/sensor"
DEVICE_API = BASE + "/api/device/pending"

LINE_RE = re.compile(r"^S,(-?\d+),(\d+),(\d+),(\d+)$")

conn = None
addr = None
buf = ""


def wait_for_connection():
    global conn, addr
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(1)
    print(f"[桥] TCP 服务器已启动，监听端口 {PORT}，等模块连接...")
    while True:
        conn, addr = s.accept()
        print(f"[桥] 模块已连接！IP = {addr[0]}")
        return


def recv_loop():
    global conn, buf
    count = 0
    while True:
        try:
            data = conn.recv(1024)
        except Exception:
            print("\n[桥] 模块断开了，等待重连...")
            buf = ""
            wait_for_connection()
            continue
        if not data:
            print("\n[桥] 模块断开了，等待重连...")
            buf = ""
            wait_for_connection()
            continue

        buf += data.decode("utf-8", errors="ignore")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip("\r").strip()
            m = LINE_RE.match(line)
            if m:
                temp, light, pot, freq = m.groups()
                payload = {
                    "temperature": float(temp) / 100.0,
                    "light": float(light) / 1000.0,
                    "pot": float(pot) / 1000.0,
                    "frequency": int(freq),
                }
                try:
                    r = requests.post(API, json=payload, timeout=3)
                    count += 1
                    print(f"[{time.strftime('%H:%M:%S')}] #{count} {line} -> HTTP {r.status_code}")
                except Exception as e:
                    print(f"[{time.strftime('%H:%M:%S')}] {line} -> 转发失败: {e}")
            else:
                if line:
                    print(f"[桥] 忽略: {line!r}")


def poll_loop():
    global conn
    last_poll = 0
    while True:
        if conn is None:
            time.sleep(0.5)
            continue
        now = time.time()
        if now - last_poll >= 0.5:
            last_poll = now
            try:
                r = requests.get(DEVICE_API, timeout=2)
                cmd = r.json().get("cmd", "")
                if cmd:
                    try:
                        conn.send((cmd + "\r\n").encode("utf-8"))
                        print(f"[{time.strftime('%H:%M:%S')}] 命令下发: {cmd}")
                    except Exception:
                        pass
            except Exception:
                pass
        time.sleep(0.05)


if __name__ == "__main__":
    wait_for_connection()
    t1 = threading.Thread(target=recv_loop, daemon=True)
    t1.start()
    t2 = threading.Thread(target=poll_loop, daemon=True)
    t2.start()
    print(f"[桥] 上行转发到 {API}，下行轮询 {DEVICE_API}（Ctrl+C 退出）")
    while True:
        time.sleep(1)
