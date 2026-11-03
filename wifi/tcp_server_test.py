"""
ESP-01S TCP 桥（双向）
- 等模块连接
- 收到数据打印
- 键盘输入命令发给模块：R1=1 / R1=0 / B1=1 / B1=0
"""
import socket
import threading

HOST = "0.0.0.0"
PORT = 8080

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(1)
print(f"TCP 服务器已启动，监听端口 {PORT}，等模块连接...")

conn, addr = s.accept()
print(f"模块已连接！IP = {addr[0]}")
print("输入命令：R1=1(开继电器) R1=0(关) B1=1(开蜂鸣器) B1=0(关)")

def recv_loop():
    while True:
        data = conn.recv(1024)
        if not data:
            print("\n模块断开了")
            break
        print(f"\n收到: {data}")

t = threading.Thread(target=recv_loop, daemon=True)
t.start()

while True:
    cmd = input("> ")
    if cmd.strip():
        conn.send((cmd + "\r\n").encode())
