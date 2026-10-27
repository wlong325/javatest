package com.example.javatest.controller;

import org.springframework.web.bind.annotation.*;

import java.util.Map;
import java.util.Queue;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * 双向控制：浏览器按钮 -> 命令队列 -> 桥轮询取走 -> 写串口 -> 板子执行。
 * 命令协议（桥 -> 板）：R1/R0 继电器开/关，B1/B0 蜂鸣器开/关。
 */
@RestController
@RequestMapping("/api/device")
public class DeviceController {

    /** 待下发命令队列（内存，重启清空，与 DataCleanRunner 的"启动清库"一致） */
    private final Queue<String> commandQueue = new ConcurrentLinkedQueue<>();

    /** 最近一条命令及其状态，供前端展示 */
    private volatile String lastCommand = "";
    private volatile String lastStatus = "";   // PENDING=待下发，SENT=桥已取走写串口
    private volatile long lastTime = 0;

    @PostMapping("/command")
    public String command(@RequestBody Map<String, String> body) {
        String relay = body.get("relay");
        String buzzer = body.get("buzzer");

        String cmd = null;
        if ("on".equals(relay)) cmd = "R1";
        else if ("off".equals(relay)) cmd = "R0";
        else if ("on".equals(buzzer)) cmd = "B1";
        else if ("off".equals(buzzer)) cmd = "B0";

        if (cmd == null) return "未知命令（relay: on/off，buzzer: on/off）";
        commandQueue.offer(cmd);
        lastCommand = cmd;
        lastStatus = "PENDING";
        lastTime = System.currentTimeMillis();
        return "已加入队列: " + cmd;
    }

    /** 桥每 500ms 轮询取命令（取到即出队） */
    @GetMapping("/pending")
    public Map<String, String> pending() {
        String cmd = commandQueue.poll();
        if (cmd == null) return Map.of("cmd", "");
        lastStatus = "SENT";
        return Map.of("cmd", cmd);
    }

    /** 前端状态展示 */
    @GetMapping("/status")
    public Map<String, Object> status() {
        return Map.of(
                "lastCommand", lastCommand,
                "status", lastStatus,
                "time", lastTime
        );
    }
}
