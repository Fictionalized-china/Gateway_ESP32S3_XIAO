# SmartReef ESP32S3 XIAO 家庭网关

基于 ESP-IDF 的 SmartReefTank 家庭网关固件，用于在云端接口和本地鱼缸 BLE 主控之间做中转。

本项目面向 Seeed Studio XIAO ESP32S3 基础板。智能设备主控不启用 WiFi，也不直接访问云端；网关通过 WiFi 访问云端，通过 BLE Central 连接智能设备主控。

## 项目状态

当前是第一版可用网关固件

- WiFi 连接由 `idf.py menuconfig` 配置。
- 不做 AP 配网。
- 不做本地 HTTP 配置页。
- 不做本地 Web 控制台。
- 使用 HTTPS 轮询云端命令和上传状态。
- 使用 ESP-IDF x509 certificate bundle 做服务器证书校验。
- 代码结构保留后续扩展多设备的空间。

## 系统架构

```text
小程序 / Web
    |
    | HTTPS
    v
云端 API
    |
    | HTTPS polling / upload
    v
ESP32S3 XIAO 网关
    |
    | BLE Central
    v
智能设备主控
```

网关承担以下职责：

- 连接家庭 WiFi。
- 生成稳定的 `gateway_id`。
- 扫描并连接 智能设备 BLE 主控。
- 读取 BLE Identity，确认真实 `device_id` 和 `reg_code`。
- 订阅 BLE 状态通知并解析智能设备状态。
- 定时上传状态到云端。
- 定时从云端拉取待执行命令。
- 将云端命令写入智能设备 BLE Command Characteristic。
- 将本地执行结果回传给云端。

## 技术栈

- 框架：ESP-IDF v6.0.1
- 芯片目标：ESP32-S3
- 开发语言：C
- 构建系统：ESP-IDF CMake
- BLE 协议栈：NimBLE
- HTTP 客户端：`esp_http_client`
- TLS 证书：`esp_crt_bundle`
- JSON：`espressif/cjson`
- 配置方式：ESP-IDF Kconfig / `idf.py menuconfig`
- 分区表：自定义 `partitions.csv`

## 目录结构

```text
.
|-- CMakeLists.txt
|-- README.md
|-- dependencies.lock
|-- main
|   |-- CMakeLists.txt
|   |-- Kconfig.projbuild
|   |-- app_config.h
|   |-- app_main.c
|   |-- ble_smartreef.c
|   |-- ble_smartreef.h
|   |-- cloud_client.c
|   |-- cloud_client.h
|   |-- gateway_core.c
|   |-- gateway_core.h
|   |-- gateway_id.c
|   |-- gateway_id.h
|   |-- gateway_types.h
|   |-- idf_component.yml
|   |-- serial_console.c
|   |-- serial_console.h
|   |-- status_codec.c
|   |-- status_codec.h
|   |-- wifi_manager.c
|   `-- wifi_manager.h
|-- partitions.csv
|-- sdkconfig.defaults
`-- sdkconfig
```

主要模块说明：

- `app_main.c`：固件入口，初始化 NVS、串口命令和网关核心。
- `gateway_core.c`：主调度逻辑，负责周期上传、周期轮询、重连协调。
- `wifi_manager.c`：WiFi STA 初始化、连接和重连。
- `gateway_id.c`：根据 WiFi STA MAC 生成稳定 `gateway_id`。
- `ble_smartreef.c`：SmartReef BLE Central 扫描、连接、服务发现、读写和通知处理。
- `status_codec.c`：解析 SmartReef 11 字节二进制状态包。
- `cloud_client.c`：云端 HTTPS API 封装。
- `serial_console.c`：串口调试命令。
- `Kconfig.projbuild`：项目自定义 `menuconfig` 配置项。

## BLE 协议

网关扫描 BLE 广播名：

```text
SmartReef-*
```

Service UUID：

```text
0000FFF0-0000-1000-8000-00805F9B34FB
```

Characteristic：

| 用途 | UUID | 属性 | 数据格式 |
| --- | --- | --- | --- |
| Command | `0000FFF1-0000-1000-8000-00805F9B34FB` | WRITE | ASCII JSON |
| Status | `0000FFF2-0000-1000-8000-00805F9B34FB` | READ / NOTIFY | 11 字节二进制 |
| Identity | `0000FFF3-0000-1000-8000-00805F9B34FB` | READ | ASCII 文本 |

Identity 格式：

```text
I<device_id>,<reg_code>
```

示例：

```text
I441BF6FE7C4C,004120
```

Status 状态包格式：

| 字节 | 含义 |
| --- | --- |
| byte0 | 固定 `0x53`，字符 `S` |
| byte1 | 协议版本，当前为 `1` |
| byte2-3 | `temp_x10`，小端 int16 |
| byte4-5 | `ph_x100`，小端 uint16 |
| byte6 | `water_ok`，1 正常，0 缺水 |
| byte7 bit0 | 灯光状态 |
| byte7 bit1 | 加热状态 |
| byte7 bit2 | 水泵状态 |
| byte8 | 控制模式，1 AUTO，0 MANUAL |
| byte9 | 告警开关，1 开，0 关 |
| byte10 | 状态序号 `seq` |

## 云端接口

默认云端地址：

```text
https://api.tap041120.online
```

网关使用的请求头：

```text
X-Gateway-Key: <通过 menuconfig 配置>
Content-Type: application/json
```

已实现接口：

- `POST /gateway/upload`
- `GET /gateway/cmd?gateway_id=<gateway_id>&device_id=<device_id>`
- `POST /gateway/cmd/result`

## 支持命令

当前支持将以下云端命令转发给鱼缸 BLE 主控：

```json
{"cmd":"light_on"}
{"cmd":"light_off"}
{"cmd":"pump_on"}
{"cmd":"pump_off"}
{"cmd":"heater_on"}
{"cmd":"heater_off"}
{"cmd":"mode_auto"}
{"cmd":"mode_manual"}
{"cmd":"alarm_enable"}
{"cmd":"alarm_disable"}
```

命令执行结果策略：

- BLE 写入成功后，上报云端 `success`。
- BLE 未连接、写入失败或命令不支持时，上报云端 `failed`。
- 命令是否真正引发状态变化，通过后续 BLE notify 和状态上传观察。

## 配置说明

运行：

```powershell
idf.py menuconfig
```

进入：

```text
SmartReef Gateway
```

可配置项：

| 配置项 | 说明 | 默认值 |
| --- | --- | --- |
| `GATEWAY_WIFI_SSID` | 家庭 WiFi 名称 | 空 |
| `GATEWAY_WIFI_PASSWORD` | 家庭 WiFi 密码 | 空 |
| `GATEWAY_LOG_WIFI_PASSWORD` | 是否在串口打印明文 WiFi 密码 | 关闭 |
| `GATEWAY_CLOUD_BASE_URL` | 云端 API 地址 | `https://api.tap041120.online` |
| `GATEWAY_KEY` | 网关访问云端的 Key | 演示 Key |
| `GATEWAY_UPLOAD_INTERVAL_MS` | 状态上传周期 | `3000` |
| `GATEWAY_CMD_POLL_INTERVAL_MS` | 命令轮询周期 | `1000` |
| `GATEWAY_HTTP_TIMEOUT_MS` | HTTP 请求超时 | `8000` |
| `GATEWAY_BLE_SCAN_PREFIX` | BLE 扫描名前缀 | `SmartReef-` |
| `GATEWAY_COMMAND_TIMEOUT_MS` | 命令观察窗口 | `5000` |
| `GATEWAY_MAX_DEVICES` | 设备表容量 | `4` |


## 分区表

项目使用自定义分区表 `partitions.csv`：

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x300000,
```

原因：

- 默认 ESP-IDF single app 分区约 1MB，不足以容纳当前固件。
- 当前 factory app 分区为 3MB，构建后仍有较大余量。
- `sdkconfig.defaults` 默认配置 8MB Flash，适配 XIAO ESP32S3 常见版本。

## 构建

先加载 ESP-IDF v6.0.1 环境：

```powershell
. C:\esp\v6.0.1\esp-idf\export.ps1
```

配置：

```powershell
idf.py menuconfig
```

构建：

```powershell
idf.py build
```

烧录：

```powershell
idf.py -p COMx flash
```

监视串口：

```powershell
idf.py -p COMx monitor
```

烧录并打开监视：

```powershell
idf.py -p COMx flash monitor
```

其中 `COMx` 替换为你的实际串口号。

## 串口调试命令

固件启动后，可以在串口监视器输入以下命令：

| 命令 | 说明 |
| --- | --- |
| `status` | 打印当前网关、WiFi、BLE、设备和堆内存状态 |
| `upload` | 立即上传一次当前状态 |
| `poll` | 立即轮询一次云端命令 |
| `reconnect` | 重新连接 WiFi 和 BLE |
| `devices` | 打印已发现设备信息 |

串口日志会输出：

- WiFi 配置摘要。
- WiFi 连接状态。
- 当前 `gateway_id`。
- BLE 扫描、连接和断开。
- 读到的 `device_id`、`reg_code` 和 `device_type`。
- 状态包解析结果。
- HTTPS 请求状态。
- 命令轮询结果。
- 命令执行和回传结果。
- 当前堆内存余量。

## 常见问题

### 1. 构建提示 app partition too small

请确认已经使用项目自带 `partitions.csv`，并且 `sdkconfig.defaults` 中存在：

```text
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
```

然后重新配置并构建：

```powershell
idf.py reconfigure
idf.py build
```

### 2. WiFi 已连接但 HTTPS 请求失败

如果日志包含：

```text
WiFi connected, IP ...
esp-x509-crt-bundle: No matching trusted root certificate found
```

说明 WiFi 已经连上，问题在 TLS 证书链验证。

项目已启用：

```text
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y
```

请重新 `idf.py reconfigure && idf.py build && idf.py flash`，确保新配置已烧录。

### 3. 串口反复出现 uart driver error

较早版本可能没有安装 UART driver 就读取串口。

当前版本已在 `serial_console_start()` 中调用 `uart_driver_install()`，重新构建烧录即可。

### 4. 智能设备重启后网关是否会自动恢复

会。日志中应能看到类似流程：

```text
BLE disconnected
Scanning for BLE devices
Found SmartReef advertisement
BLE connected
identity device_id=...
subscribe status
notify temp=...
```

只要智能设备 BLE 广播恢复，网关会重新连接并重新订阅状态。

### 5. 日志显示 poll/upload skipped

如果 WiFi 或 BLE 设备尚未就绪，网关会跳过周期轮询或上传。这是正常保护逻辑，不代表固件崩溃。

