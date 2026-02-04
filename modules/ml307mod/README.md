# ML307 MicroPython Module

MicroPython 模块，用于 ML307/EC801E/NT26K 4G Cat.1 模组的网络访问。

## 支持的模组

- ML307R / ML307A
- EC801E
- NT26K

## 功能特性

- 自动模组检测
- **单例模式** - UART 和 Modem 自动缓存，防止重复初始化
- HTTP / HTTPS 请求
- WebSocket 客户端
- TCP / SSL TCP 连接
- UDP 通信
- MQTT 客户端

## 快速开始

### 初始化模组

```python
import ml307
import time

# 创建 Modem 对象（单例模式：多次调用返回同一个实例）
modem = ml307.Modem(tx_pin=17, rx_pin=18, dtr_pin=8, baud_rate=115200)

# 或者获取已初始化的 modem（如果存在）
if ml307.is_initialized():
    modem = ml307.get_modem()
    print("使用已缓存的 Modem 实例")
else:
    modem = ml307.Modem(tx_pin=17, rx_pin=18, dtr_pin=8, baud_rate=115200)

# 等待网络就绪
status = modem.wait_for_network(timeout_ms=30000)

if status == ml307.STATUS_READY:
    print("网络已连接!")
    print("IMEI:", modem.imei())
    print("ICCID:", modem.iccid())
    print("运营商:", modem.carrier())
    print("信号强度:", modem.csq())
    print("固件版本:", modem.revision())
elif status == ml307.STATUS_ERROR_INSERT_PIN:
    print("SIM卡未插入或PIN码错误")
elif status == ml307.STATUS_ERROR_TIMEOUT:
    print("网络连接超时")
else:
    print("网络连接失败, 状态码:", status)
```

### 单例模式说明

由于 UART 硬件资源不能重复初始化，`ml307` 模块实现了单例缓存机制：

```python
import ml307

# 第一次创建 - 真正初始化硬件
modem1 = ml307.Modem(tx_pin=17, rx_pin=18, baud_rate=115200)

# 第二次创建 - 返回缓存的同一实例，不会重复初始化 UART
modem2 = ml307.Modem(tx_pin=17, rx_pin=18, baud_rate=115200)

print(modem1 is modem2)  # True - 是同一个对象

# 检查是否已初始化
print(ml307.is_initialized())  # True

# 获取已缓存的 modem
modem3 = ml307.get_modem()
print(modem3 is modem1)  # True

# 如果需要强制重新初始化（谨慎使用！）
# ml307.force_destroy()
# modem_new = ml307.Modem(tx_pin=17, rx_pin=18, baud_rate=115200)
```

### HTTP 请求

```python
# 创建 HTTP 客户端
http = modem.create_http()

# 设置请求头
http.set_header("User-Agent", "MicroPython/1.0")
http.set_timeout(10000)

# GET 请求
if http.open("GET", "https://httpbin.org/json"):
    print("状态码:", http.status_code())
    print("响应长度:", http.body_length())
    print("响应内容:", http.read_all())
    http.close()

# POST 请求
http.set_header("Content-Type", "application/json")
http.set_content('{"key": "value"}')
if http.open("POST", "https://httpbin.org/post"):
    print("响应:", http.read_all())
    http.close()
```

### WebSocket 连接

```python
# 创建 WebSocket 客户端
ws = modem.create_websocket()

# 设置回调函数
def on_data(data, binary):
    print("收到数据:", data)

def on_connected():
    print("WebSocket 已连接")

def on_disconnected():
    print("WebSocket 已断开")

def on_error(error):
    print("WebSocket 错误:", error)

ws.on_data(on_data)
ws.on_connected(on_connected)
ws.on_disconnected(on_disconnected)
ws.on_error(on_error)

# 设置请求头
ws.set_header("Protocol-Version", "3")

# 连接
if ws.connect("wss://echo.websocket.org/"):
    # 发送消息
    ws.send('{"type": "ping"}')
    ws.send(b'\x01\x02\x03', binary=True)
    
    # 发送 ping
    ws.ping()
    
    # 等待接收
    time.sleep(5)
    
    ws.close()
```

### TCP 连接

```python
# 创建 TCP 客户端 (普通TCP)
tcp = modem.create_tcp()

# 或创建 SSL TCP 客户端
tcp_ssl = modem.create_tcp(use_ssl=True)

# 设置回调函数
def on_data(data):
    print("TCP 收到:", data)

def on_disconnected():
    print("TCP 已断开")

tcp.on_data(on_data)
tcp.on_disconnected(on_disconnected)

# 连接
if tcp.connect("httpbin.org", 80):
    # 发送 HTTP 请求
    request = "GET /ip HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n"
    sent = tcp.send(request)
    print("发送了", sent, "字节")
    
    # 等待响应
    time.sleep(3)
    
    tcp.disconnect()
```

### UDP 通信

```python
# 创建 UDP 客户端
udp = modem.create_udp()

# 设置回调函数
def on_message(data):
    print("UDP 收到:", data)

udp.on_message(on_message)

# 连接到 UDP 服务器
if udp.connect("8.8.8.8", 53):
    # 发送数据
    sent = udp.send(b"Hello UDP!")
    print("发送了", sent, "字节")
    
    time.sleep(2)
    
    udp.disconnect()
```

### MQTT 客户端

```python
# 创建 MQTT 客户端
mqtt = modem.create_mqtt()

# 设置回调函数
def on_message(topic, payload):
    print("MQTT 消息 [{}]: {}".format(topic, payload))

def on_connected():
    print("MQTT 已连接")
    # 连接后订阅主题
    mqtt.subscribe("test/esp32/message")

def on_disconnected():
    print("MQTT 已断开")

def on_error(error):
    print("MQTT 错误:", error)

mqtt.on_message(on_message)
mqtt.on_connected(on_connected)
mqtt.on_disconnected(on_disconnected)
mqtt.on_error(on_error)

# 设置 KeepAlive
mqtt.set_keepalive(60)

# 连接到 MQTT 代理
if mqtt.connect("broker.emqx.io", 1883, "esp32_client", "username", "password"):
    # 发布消息
    mqtt.publish("test/esp32/hello", "Hello from ESP32!", qos=0)
    
    # 等待消息
    time.sleep(10)
    
    # 取消订阅
    mqtt.unsubscribe("test/esp32/message")
    
    mqtt.disconnect()
```

## API 参考

### 模块级函数

| 函数 | 描述 |
|------|------|
| `ml307.is_initialized()` | 检查 Modem 是否已初始化 |
| `ml307.get_modem()` | 获取已缓存的 Modem 实例，未初始化返回 None |
| `ml307.force_destroy()` | 强制销毁 Modem 并清除缓存（谨慎使用） |

### ml307.Modem

Modem 主类，用于初始化和管理 4G 模组。

**注意**: Modem 采用单例模式，多次创建会返回同一个实例，避免 UART 重复初始化问题。

#### 构造函数

```python
Modem(tx_pin, rx_pin, dtr_pin=-1, baud_rate=115200, timeout_ms=-1)
```

**参数:**
- `tx_pin` (int): UART TX 引脚号
- `rx_pin` (int): UART RX 引脚号
- `dtr_pin` (int, 可选): DTR 引脚号，用于低功耗唤醒，默认 -1 (不使用)
- `baud_rate` (int, 可选): UART 波特率，默认 115200
- `timeout_ms` (int, 可选): 检测超时时间，默认 -1 (自动)

#### 方法

| 方法 | 描述 |
|------|------|
| `wait_for_network(timeout_ms=30000)` | 等待网络就绪，返回状态码 |
| `is_ready()` | 检查网络是否就绪 |
| `reboot()` | 重启模组 |
| `imei()` | 获取 IMEI |
| `iccid()` | 获取 ICCID |
| `carrier()` | 获取运营商名称 |
| `csq()` | 获取信号强度 (0-31) |
| `revision()` | 获取固件版本 |
| `close()` | 关闭并释放资源 |
| `create_http()` | 创建 HTTP 客户端 |
| `create_websocket()` | 创建 WebSocket 客户端 |
| `create_tcp(use_ssl=False)` | 创建 TCP 客户端 |
| `create_udp()` | 创建 UDP 客户端 |
| `create_mqtt()` | 创建 MQTT 客户端 |

### ml307.Http

HTTP 客户端类。

#### 方法

| 方法 | 描述 |
|------|------|
| `set_timeout(timeout_ms)` | 设置超时时间 |
| `set_header(key, value)` | 设置请求头 |
| `set_content(data)` | 设置请求体 |
| `open(method, url)` | 发送请求，返回 True/False |
| `close()` | 关闭连接 |
| `status_code()` | 获取响应状态码 |
| `body_length()` | 获取响应体长度 |
| `read(size=1024)` | 读取响应数据 |
| `read_all()` | 读取全部响应 |
| `last_error()` | 获取最后错误码 |

### ml307.WebSocket

WebSocket 客户端类。

#### 方法

| 方法 | 描述 |
|------|------|
| `set_header(key, value)` | 设置请求头 |
| `connect(url)` | 连接到 WebSocket 服务器 |
| `send(data, binary=False)` | 发送数据 |
| `ping()` | 发送 ping 帧 |
| `close()` | 关闭连接 |
| `is_connected()` | 检查连接状态 |
| `on_data(callback)` | 设置数据回调: `callback(data, binary)` |
| `on_connected(callback)` | 设置连接回调 |
| `on_disconnected(callback)` | 设置断开回调 |
| `on_error(callback)` | 设置错误回调: `callback(error)` |
| `last_error()` | 获取最后错误码 |

### ml307.Tcp

TCP 客户端类。

#### 方法

| 方法 | 描述 |
|------|------|
| `connect(host, port)` | 连接到服务器 |
| `disconnect()` | 断开连接 |
| `send(data)` | 发送数据，返回发送字节数 |
| `is_connected()` | 检查连接状态 |
| `on_data(callback)` | 设置数据回调: `callback(data)` |
| `on_disconnected(callback)` | 设置断开回调 |
| `last_error()` | 获取最后错误码 |

### ml307.Udp

UDP 客户端类。

#### 方法

| 方法 | 描述 |
|------|------|
| `connect(host, port)` | 连接到服务器 |
| `disconnect()` | 断开连接 |
| `send(data)` | 发送数据，返回发送字节数 |
| `is_connected()` | 检查连接状态 |
| `on_message(callback)` | 设置消息回调: `callback(data)` |
| `last_error()` | 获取最后错误码 |

### ml307.Mqtt

MQTT 客户端类。

#### 方法

| 方法 | 描述 |
|------|------|
| `set_keepalive(seconds)` | 设置 KeepAlive 时间 |
| `connect(broker, port, client_id, username="", password="")` | 连接到 MQTT 代理 |
| `disconnect()` | 断开连接 |
| `publish(topic, payload, qos=0)` | 发布消息 |
| `subscribe(topic, qos=0)` | 订阅主题 |
| `unsubscribe(topic)` | 取消订阅 |
| `is_connected()` | 检查连接状态 |
| `on_message(callback)` | 设置消息回调: `callback(topic, payload)` |
| `on_connected(callback)` | 设置连接回调 |
| `on_disconnected(callback)` | 设置断开回调 |
| `on_error(callback)` | 设置错误回调: `callback(error)` |
| `last_error()` | 获取最后错误码 |

## 状态常量

| 常量 | 值 | 描述 |
|------|---|------|
| `STATUS_READY` | 0 | 网络就绪 |
| `STATUS_ERROR` | 1 | 一般错误 |
| `STATUS_ERROR_INSERT_PIN` | -1 | SIM卡/PIN 错误 |
| `STATUS_ERROR_REGISTRATION_DENIED` | -2 | 网络注册被拒绝 |
| `STATUS_ERROR_TIMEOUT` | -3 | 连接超时 |

## 注意事项

1. 确保使用正确的 UART 引脚
2. EC801E 模组需要确认固件是否支持 SSL TCP
3. 建议在使用完毕后调用 `close()` 释放资源
4. 回调函数在 C 层执行，避免在回调中执行耗时操作
5. 低功耗模式需要正确配置 DTR/RI 引脚

## 许可证

MIT License

## 作者

- zengkv
- 基于 esp-ml307 库 by 虾哥 Terrence
