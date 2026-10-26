# -*- coding: utf-8 -*-
"""
串口 -> HTTP 桥：读板子串口数据，转发到 Spring Boot 后端
用法：
    pip install pyserial requests
    python serial_forward.py [COM口] [波特率] [后端地址]
    例：python serial_forward.py COM6 9600 http://localhost:8080
板子数据格式（模板 C 代码主动上报，均为整数）：
    S,<温度×100>,<光敏mV>,<电位器mV>,<频率Hz>\r\n
    例：S,2706,2647,5000,142   （2706=27.06℃，2647mV=2.647V）
桥负责单位换算：温度÷100→℃，光敏/电位器÷1000→V
"""
import serial
import requests
import re
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
BASE = sys.argv[3] if len(sys.argv) > 3 else "http://localhost:8080"
API = BASE + "/api/sensor"

LINE_RE = re.compile(r"^S,(-?\d+),(\d+),(\d+),(\d+)$")


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    print(f"[桥] 已打开 {PORT} @ {BAUD}，转发到 {API}（Ctrl+C 退出）")
    buf = ""
    count = 0
    while True:
        try:
            data = ser.read(ser.in_waiting or 1)
        except serial.SerialException as e:
            print(f"[桥] 串口读失败: {e}（板子拔了？）")
            time.sleep(2)
            continue
        if data:
            buf += data.decode("utf-8", errors="ignore")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip("\r").strip()
                m = LINE_RE.match(line)
                if m:
                    temp, light, pot, freq = m.groups()
                    payload = {
                        "temperature": float(temp) / 100.0,   # 2706 -> 27.06 ℃
                        "light": float(light) / 1000.0,       # 2647 -> 2.647 V
                        "pot": float(pot) / 1000.0,           # 5000 -> 5.000 V
                        "frequency": int(freq),
                    }
                    try:
                        r = requests.post(API, json=payload, timeout=3)
                        count += 1
                        print(f"[{time.strftime('%H:%M:%S')}] #{count} {line} -> HTTP {r.status_code} {r.text}")
                    except Exception as e:
                        print(f"[{time.strftime('%H:%M:%S')}] {line} -> 转发失败: {e}")
                else:
                    print(f"[桥] 忽略非数据行: {line!r}")
        time.sleep(0.05)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[桥] 已退出")
    except serial.SerialException as e:
        print(f"[桥] 打不开 {PORT}: {e}（请确认串口已开、参数正确）")
