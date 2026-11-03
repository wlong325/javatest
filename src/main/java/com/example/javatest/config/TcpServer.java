package com.example.javatest.config;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.context.event.ApplicationReadyEvent;
import org.springframework.context.event.EventListener;
import org.springframework.stereotype.Component;
import org.springframework.web.client.RestTemplate;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * TCP Server：直接接收 ESP-01S 板子数据，代替原来的 Python 桥。
 * 上行：解析 S,... 格式 → 转发到 /api/sensor
 * 下行：定时轮询 /api/device/pending → 发给板子
 */
@Component
public class TcpServer {

    private static final int TCP_PORT = 8888;
    private static final String BASE_HTTP = "http://localhost:8080";
    private static final Pattern LINE_RE = Pattern.compile("^S,(-?\\d+),(\\d+),(\\d+),(\\d+)$");

    private final RestTemplate restTemplate = new RestTemplate();
    private Socket clientSocket;
    private OutputStream clientOut;
    private boolean running = false;

    @EventListener(ApplicationReadyEvent.class)
    public void start() {
        running = true;
        new Thread(this::acceptLoop, "tcp-accept").start();
        new Thread(this::pollLoop, "tcp-poll").start();
        System.out.println("[TCP] 服务器已启动，监听端口 " + TCP_PORT);
    }

    private void acceptLoop() {
        try (ServerSocket server = new ServerSocket(TCP_PORT)) {
            while (running) {
                Socket sock = server.accept();
                System.out.println("[TCP] 板子已连接！IP = " + sock.getInetAddress().getHostAddress());
                clientSocket = sock;
                clientOut = sock.getOutputStream();
                handleClient(sock);
            }
        } catch (Exception e) {
            System.err.println("[TCP] 服务器异常: " + e.getMessage());
        }
    }

    private void handleClient(Socket sock) {
        try {
            BufferedReader in = new BufferedReader(new InputStreamReader(sock.getInputStream()));
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;

                Matcher m = LINE_RE.matcher(line);
                if (m.matches()) {
                    String temp = m.group(1);
                    String light = m.group(2);
                    String pot = m.group(3);
                    String freq = m.group(4);

                    Map<String, Object> payload = new HashMap<>();
                    payload.put("temperature", Double.parseDouble(temp) / 100.0);
                    payload.put("light", Double.parseDouble(light) / 1000.0);
                    payload.put("pot", Double.parseDouble(pot) / 1000.0);
                    payload.put("frequency", Integer.parseInt(freq));

                    try {
                        restTemplate.postForObject(BASE_HTTP + "/api/sensor", payload, String.class);
                        System.out.println("[TCP] 上行: " + line);
                    } catch (Exception e) {
                        System.err.println("[TCP] 上行失败: " + e.getMessage());
                    }
                }
                // 其他数据（如 AT 响应）忽略
            }
        } catch (Exception e) {
            System.out.println("[TCP] 板子断开了");
        } finally {
            clientSocket = null;
            clientOut = null;
        }
    }

    private void pollLoop() {
        while (running) {
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                break;
            }

            if (clientOut == null) continue;

            try {
                Map resp = restTemplate.getForObject(BASE_HTTP + "/api/device/pending", Map.class);
                String cmd = (String) resp.get("cmd");
                if (cmd != null && !cmd.isEmpty()) {
                    clientOut.write((cmd + "\r\n").getBytes());
                    clientOut.flush();
                    System.out.println("[TCP] 下行命令: " + cmd);
                }
            } catch (Exception e) {
                // 忽略轮询错误
            }
        }
    }
}
