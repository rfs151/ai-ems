# 储能 EMS 边缘网关演示

部署在储能站现场的 EMS 边缘网关：多厂商设备 Modbus 接入、功率控制、本地存储、云边协同。
设备为仿真，厂商名称均为虚构。详细设计见 [docs/DESIGN.md](docs/DESIGN.md)。

```
device-sim (Python)  ──Modbus TCP──▶  ems-core (C99)  ──SQLite 发件箱──▶  ems-sync (C99)
                                         采集/控制/存储                      MQTT 上云
                                                                              │
                     浏览器  ◀──WebSocket──  cloud (FastAPI + Vue 页面)  ◀──mosquitto
```

## 进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 设备仿真、采集/实时库/存储、上云进程、云端、总览页和点表页 | ✅ 完成 |
| 2 | FCR / mFRR / LFSM-O 控制器、指令下发链路、演示控制台、控制算法页、单元测试 | ✅ 完成 |
| 3 | 规则引擎、断网补传、MQTT TLS、故障注入 | |
| 4 | 104 接入 ai-scada、固件签名、aarch64 交叉编译 CI；之后 61850 | |

## 用 Docker 运行

```bash
docker compose up -d --build          # 页面：http://127.0.0.1:8095
```

国内网络可在 `.env` 里设置 `REGISTRY=docker.1ms.run`、`PIP_INDEX_URL`、`NPM_REGISTRY`。

## 本地开发

依赖：`gcc make libmodbus-dev libmosquitto-dev libsqlite3-dev mosquitto`，Python 3.12，Node 22。

```bash
python3 -m venv .venv && .venv/bin/pip install pymodbus==3.7.4 fastapi "uvicorn[standard]" paho-mqtt
mosquitto -d                                                  # 本地 broker :1883

cd device-sim && MQTT_HOST=localhost ../.venv/bin/python -m sim          # 设备仿真 :15021-15026
cd ems-core && make && ./build/ems-core -c config                         # 边缘主进程
cd ems-core && ./build/ems-sync -c config                                 # 上云进程
cd cloud && ../.venv/bin/uvicorn app:app --port 8095                      # 云端
cd web && npm install && npm run dev                                      # 前端热更新 :5173
```

单元测试：`make test`（58 项断言）；`make test-asan` 在 ASan/UBSan 下运行。

内存检查：`make asan` 后运行 `build-asan/` 下的程序。本机内核 `vm.mmap_rnd_bits=32` 时 ASan 偶发死循环，
用 `setarch -R ./build-asan/ems-core -c config` 运行即可。

故障注入（页面上的演示控制台也可以操作）：

```bash
mosquitto_pub -t sim/scenario -m '{"action":"comm","device":"bat2","down":true}'   # 电池簇-2 通信中断
mosquitto_pub -t sim/scenario -m '{"action":"freq_step","delta_hz":0.3}'          # 频率阶跃 +0.3Hz
mosquitto_pub -t sim/scenario -m '{"action":"di","name":"estop","value":true}'    # 急停
```

## 目录

```
docs/DESIGN.md          详细设计
device-sim/sim/         plant.py 物理模型；devices.py 三家虚构厂商的寄存器布局
ems-core/
  config/               site.json 站点设备；vendors/*.json 各厂商点表
  src/config            点表解析、寄存器编解码（字序、倍率、位、枚举）
  src/acq               每设备一个采集线程，批量读合并、离线判定、写队列
  src/rtdb              实时库（读写锁 + 快照）
  src/control           algo.c：FCR / LFSM-O / mFRR 纯函数算法；control.c：叠加、联锁、SOC 限幅、爬坡、PCS 分配
  src/cmd               本地指令通道（Unix 数据报套接字，ems-sync → ems-core）
  src/storage           SQLite WAL：遥测、事件、上云发件箱
  src/alarm             异步事件队列
  src/sync              ems-sync 上云进程；下行指令校验过期、去重后转发，回 cmd_ack
  tests/                算法单元测试
cloud/app.py            云端：MQTT 订阅、历史缓存、WebSocket、指令下发与时延统计、限流、无人时自动演示
web/                    Vue 3 + ECharts 演示页
```

## 与设计稿的差异

- 日志暂用自写的轻量实现（`src/util/log.c`），接口与 zlog 用法相近，现场版本可替换。
- 第 1 阶段 MQTT 为明文、匿名，只在 compose 内部网络可达；TLS 在第 3 阶段加入。
