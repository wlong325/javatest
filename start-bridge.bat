@echo off
chcp 65001 >nul
cd /d E:\javatest
echo 串口桥启动中：COM6 @ 9600 -^> http://localhost:8080/api/sensor
echo 看到 [桥] 已打开 就说明在传数据了，这个窗口别关。
"C:\Users\86176\AppData\Local\Programs\Python\Python313\python.exe" "E:\javatest\serial_forward.py" COM6 9600 http://localhost:8080
pause