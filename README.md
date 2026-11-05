# 嵌入式综合环境监测台

CT107D 开发板采集四路环境数据（温度 / 光照 / 电位器 / 频率），经 WiFi 直连后端入库，网页端实时展示曲线并远程控制继电器与蜂鸣器。**全链路双向通信，无中间桥接进程。**

```
硬件层   CT107D-V40（IAP15F2K61S2）
            │  UART 115200：上行每 2 秒一帧 S,...，下行命令 R1/R0/B1/B0
            ▼
         ESP-01S（STA 模式，TCP 客户端）
            │  TCP 8888，连到后端所在电脑的局域网 IP
            ▼
后端     Spring Boot 8080
         ├── TcpServer      收发 TCP，替代原 Python 桥
         ├── REST /api/*    给网页用
         └── MyBatis ──▶ MySQL sensor_data
            ▲
            │  HTTP 8080，每 2 秒轮询
前端     index.html + app.js
```

## 一、这是什么

| 项 | 说明 |
|---|---|
| 板端 | 蓝桥杯 CT107D 开发板，主控 IAP15F2K61S2，Keil C51 编译 |
| 联网 | ESP-01S（ESP8266）以 STA 模式接入路由器，作为 TCP 客户端连到电脑 |
| 后端 | Spring Boot 3.0.2 + MyBatis，内嵌 TCP Server 监听 8888，无需 Python 桥 |
| 前端 | 原生 HTML / CSS / JS，零构建、零 CDN，手机框 + 窄屏铺满 |
| 数据库 | MySQL 8，库 `javatest`，表 `sensor_data` |

**能做什么**

- 板子每 2 秒上报一次四路数据，网页 2 秒轮询刷新，带 1 分钟走势曲线
- 网页点击开关，命令经 TCP 下发到板子，驱动继电器（R1/R0）与蜂鸣器（B1/B0）
- 双设备切换：CT107D-V40（真实数据）、CT117E-M4（展示态，数据区为空态卡）
- 个人页：设备概览、数据记录（最近 50 条）、导出 CSV

## 二、快速开始

### 环境要求

| 组件 | 版本 / 说明 |
|---|---|
| JDK | 17 |
| Maven | 3.6+（IDEA 自带亦可） |
| MySQL | 8.x，用户名默认 `root` |
| Keil C51 | 打开 `硬件层/test.uvproj` 编译 |
| STC-ISP | 烧录 IAP15F2K61S2 |
| 浏览器 | Chrome / Edge，手机可同局域网访问 |

### 第一步：建库建表

表结构全部在 **`src/main/resources/db/schema.sql`**，脚本自带建库、建表、字段注释和自检查询，直接执行即可：

```bash
# 方式一：命令行（在项目根目录执行）
mysql -u root -p < src/main/resources/db/schema.sql

# 方式二：Navicat → 右键连接 → 运行 SQL 文件 → 选中该文件
# 方式三：IDEA 打开该文件 → 全选 → 点右上角绿色三角
```

脚本里的语句都带 `IF NOT EXISTS`，重复执行不会报错，也不会清掉已有数据，所以不确定有没有建过时直接再跑一次就行。

建完确认一下：

```sql
SHOW TABLES FROM javatest;                  -- 应看到 sensor_data
SELECT COUNT(*) FROM javatest.sensor_data;  -- 新库为 0
```

> 库里如果还存在一张 `book` 表，那是仓库 `master` 分支的历史项目遗留，与本系统无关，可以忽略，也可以删掉。

### 第二步：改三处配置

部署到新网络时，这三处必须对齐，否则「网页有数据但板子连不上」或反之。

| 位置 | 改什么 | 当前值 |
|---|---|---|
| `src/main/resources/application.yml` | MySQL 密码 | `spring.datasource.password` |
| `硬件层/main.c` 的 `WiFi_Proc()` | WiFi 名称与密码 | `AT+CWJAP="Xiaomi 15","machunjie"` |
| `硬件层/main.c` 的 `WiFi_Proc()` | **后端所在电脑的局域网 IP** | `AT+CIPSTART="TCP","172.16.49.111",8888` |

> **注意 1**：ESP-01S 只支持 **2.4GHz** WiFi。手机热点若开了「5GHz 优选」会连不上。
> **注意 2**：电脑 IP 由路由器 DHCP 分配，重启后可能变化。建议在路由器里给这台电脑绑定静态 IP，否则 IP 一变就要重新烧录固件。

### 第三步：启动后端

IDEA 里运行 `JavatestApplication`，或命令行：

```bash
mvn clean package -DskipTests
java -jar target/javatest-0.0.1-SNAPSHOT.jar
```

启动后控制台应出现：

```
[启动清理] sensor_data 表已清空（每次启动执行）
[TCP] 服务器已启动，监听端口 8888
```

浏览器打开 <http://localhost:8080>。

### 第四步：烧录板子

1. Keil 打开 `硬件层/test.uvproj` → Rebuild（0 Error）
2. **拔掉 ESP-01S 模块**（它与板载 CH340 共用 P3.0/P3.1，不拔无法下载）
3. STC-ISP 烧录：芯片 `IAP15F2K61S2`、IRC `11.0592MHz`、串口 `COM6`、`P3.2/P3.3` 接地为 0，按板载 `S3` 进入下载
4. 烧完断电，插回 ESP-01S，重新上电

### 第五步：上电后必须按一下 S8

板子上电后会自动连 WiFi（约 3~8 秒），但**数据发送默认是关闭的，需要按一次 S8 才开始上报**。

| 板载现象 | 含义 |
|---|---|
| 数码管 `1111`（第 6 界面） | WiFi 未连上，正在重试（失败后约 3~15 秒自动退回复位） |
| 数码管 `8888` | WiFi 已拿到 IP，正在建立 TCP 连接 |
| 数码管 `7777` | TCP 已连上，链路就绪 |
| 按 S8 | 开始上报，此后每 2 秒一帧；L8 每成功一帧闪 0.2 秒 |
| 按 S4 | 数码管 6 个界面循环：温度 → 光敏 → 电位器 → 频率 → 上传次数 → WiFi 状态 |

> WiFi 状态在第 6 个界面，连按 5 次 S4 才看得到。L1~L6 分别对应 6 个界面常亮。

### 跑通自检（四步确认全链路）

1. 网页右上角显示「已连接」，下方时间在 6 秒内跳动 → **上行通**
2. 点一下继电器开关，板子上有「咔哒」声 → **下行通**
3. 数码管切到第 6 界面看到 `7777` → **TCP 链路稳定**
4. MySQL 里执行 `SELECT COUNT(*) FROM sensor_data;`，数字每 2 秒加 1 → **入库正常**

四项都过，整个系统就是通的。少哪一项，对照第五节排错手册。

## 三、通信协议

### 上行帧（板子 → TCP 8888，每 2 秒）

```
S,<温度×100>,<光敏mV>,<电位器mV>,<频率Hz>\r\n
例：S,2318,2234,2206,1083   →  23.18℃ / 2.234V / 2.206V / 1083Hz
```

后端 `TcpServer` 用正则 `^S,(-?\d+),(\d+),(\d+),(\d+)$` 校验，换算后写入 `sensor_data`：温度 ÷100，光敏 / 电位器 ÷1000。

### 下行命令（网页 → TCP 8888）

| 命令 | 动作 | 板端回复 |
|---|---|---|
| `R1` / `R0` | 继电器吸合 / 释放 | `OK` / `ERROR` |
| `B1` / `B0` | 蜂鸣器开 / 关 | `OK` / `ERROR` |
| `?` | 通信测试 | `OK` |

> 板子回了 `OK`，但后端目前把非 `S,` 开头的行直接忽略，所以网页只能显示「已下发」。补执行回执见「已知限制」。

### HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/sensor` | 写入一条数据（TcpServer 内部调用） |
| GET | `/api/sensor/latest` | 最新一条 |
| GET | `/api/sensor/history?limit=100` | 最近 N 条，按时间正序 |
| POST | `/api/device/command` | body：`{"relay":"on\|off"}` 或 `{"buzzer":"on\|off"}` |
| GET | `/api/device/pending` | TcpServer 每 500ms 取一次待发命令 |
| GET | `/api/device/status` | 最近一条命令及状态（PENDING / SENT） |

## 四、目录结构

```
javatest/
├── pom.xml                        Maven 配置（Spring Boot 3.0.2 / Java 17）
├── src/main/java/com/example/javatest/
│   ├── JavatestApplication.java   启动类
│   ├── config/
│   │   ├── TcpServer.java         TCP 8888 监听，代替原 Python 桥
│   │   └── DataCleanRunner.java   启动清表（演示用）
│   ├── controller/                SensorController / DeviceController
│   ├── service/ mapper/ entity/   业务层与 MyBatis 映射
│   └── demos/web/                 IDEA 自动生成的示例，可删
├── src/main/resources/
│   ├── application.yml            数据库与端口配置
│   ├── db/schema.sql              建库建表脚本（换机器时先跑它）
│   ├── mapper/SensorDataMapper.xml
│   └── static/                    前端：index.html + assets/
├── 硬件层/                         板端工程（Keil）
│   ├── main.c                     主程序，1031 行，GBK 编码
│   ├── Driver/                    ds1302 / iic / onewire / uart / ultrasonic
│   └── test.uvproj                Keil 工程
├── wifi/                          历史资料：Python 桥（已退役）、ESP-01S 测试模板
└── serial_forward.py              历史资料：串口桥（已退役，保留备查）
```

## 五、排错手册

| 症状 | 可能原因 | 处理 |
|---|---|---|
| 网页一直「等待数据」 | 板子没按 S8 | 按一下 S8 |
| | 后端没启动 / 表不存在 | 看控制台是否有 `[TCP] 服务器已启动`，检查建表 |
| 数码管一直 `1111` | WiFi 名密码错、或用的是 5GHz | 改 `AT+CWJAP`，确认 2.4GHz |
| | 电脑 IP 变了 | 重新确认 `AT+CIPSTART` 里的 IP |
| 数码管停在 `8888` | WiFi 通了但 TCP 连不上 | 后端没启动，或电脑防火墙拦了 8888 |
| 网页能用但板子没反应 | ESP 没插紧 / 供电不足 | 检查转接板 3.3V |
| 烧录失败 | ESP-01S 没拔 | 拔掉模块再烧 |
| 温度显示 85 或 0 | DS18B20 接触不良 | 检查排线，重插 |
| 继电器吸合后串口卡死 | USB 供电余量不足（继电器是电感负载） | 接 J12 外接 5V 电源 |
| 数据突然全没了 | `DataCleanRunner` 启动清表 | 见下方「已知限制」第 2 条 |
| 网页样式错乱 | 浏览器缓存了旧版 | `Ctrl + F5` 强刷 |
| 串口助手看到的是乱码 | 波特率不匹配 | 用 **115200** 8N1（旧的串口桥版本才是 9600） |

## 六、开发约定（改板端代码前必读）

`硬件层/main.c` 有三个硬性约束，违反会直接编译失败或乱码：

1. **编码 GBK(936)**，不是 UTF-8。Keil C51 只认 GBK，用 UTF-8 写回会导致中文注释乱码、字符串报错
2. **行尾 CRLF**。写成 LF 会让 Keil 报 `C103: unclosed string`
3. **C89**。变量声明必须放在代码块开头

另外：

- 局部大数组放 `xdata`（如 `xdata char RX1_Buffer[80]`），内部 RAM 很紧张
- 读写 P2 要保留低 5 位（`P2 &= 0x1f`）
- **不要用关 `EA` 的方案做临界区**：PCA 模块 2 负责数码管刷新，关中断会让数码管闪烁，已验证是死路
- Keil 构建产物（`obj/`、`*.lst`、`*.m51`、`*.uvgui.*`）已在 `.gitignore` 中屏蔽，不要提交

## 七、已知限制

按影响排序，都是「能跑但不够专业」的地方，不是故障：

1. **命令无执行回执**：只到「已下发」，板子回的 `OK` 被忽略。改法：`TcpServer` 解析非 `S,` 行并回写状态
2. **每次启动清空数据库**：`DataCleanRunner` 执行 `TRUNCATE TABLE sensor_data`，历史不留存
3. **TCP 单连接**：`acceptLoop` 里是同步阻塞调用，第二块板子（CT117E-M4）连不上，需改为每连接一线程
4. **命令队列在内存**：`ConcurrentLinkedQueue`，后端重启即丢失
5. **接口无鉴权**：同一局域网内任何人可 POST 开关继电器
6. **密码明文**：MySQL 密码在 `application.yml`，WiFi 密码在 `main.c`，均已进入 git 历史
7. **数据只增不减**：2 秒一条，一天 4.3 万条，长期运行需加清理或降采样任务

## 八、相关文档

| 文档 | 用途 | 状态 |
|---|---|---|
| `README.md`（本文） | 唯一的部署与使用说明 | 有效，随代码更新 |
| `项目状态总览.md` | 开发过程的状态记录 | 已过期：停留在 2026-10-30，仍描述「Python 串口桥」架构，**以本 README 为准** |
| `C51-双向控制交接说明.md` / `补充.md` | 板端踩坑记录与协议细节 | 历史过程文档，硬件细节仍有效 |
| `项目结构说明.md` | Spring Boot 目录结构学习笔记 | 写于项目早期，目录已扩展 |

---

**分支说明**：本仓库 `master` 分支是另一个独立项目（图书馆系统），请勿在 `master` 上开发。当前开发分支见 `git branch --show-current`。
