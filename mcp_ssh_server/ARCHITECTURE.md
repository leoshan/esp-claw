# ESP-Claw SSH MCP Gateway — 架构设计、开发心得与使用手册

## 项目概述

本项目是一个为 [ESP-Claw](https://github.com/espressif/esp-claw)（乐鑫端侧 AI Agent 框架）量身打造的 **MCP (Model Context Protocol) SSH 网关服务器**。它运行在你的 Mac / PC / 服务器上，让 ESP32 上的 AI Agent 获得通过 SSH 控制远程 Linux 服务器和 Docker 沙箱的能力。

### 项目动机

ESP-Claw 是一个运行在 ESP32-S3 芯片上的端侧 AI Agent，它内置了大模型对话、Lua 自编程、视觉识别等能力，但**原生不支持 SSH 协议**（ESP32 单片机的算力和内存无法承受 SSH 握手的非对称加密开销）。为了让 ESP-Claw 具备控制远程服务器和沙箱的能力，我们设计了这个"中转网关"架构：

```
ESP-Claw (ESP32-S3)  ───Wi-Fi HTTP───>  本服务器 (Mac/PC)  ───SSH (Paramiko)───>  远程 Linux / Docker 沙箱
```

---

## 一、 技术架构详解

### 1.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                   ESP-Claw SSH MCP Gateway Server                   │
│                     (Python 3.14 + Uvicorn)                        │
│                                                                     │
│  ┌───────────────────────┐    ┌──────────────────────────────────┐ │
│  │  MCP SSE Endpoint     │    │  HTTP REST API Endpoint          │ │
│  │  GET /sse             │    │  POST /api/ssh                   │ │
│  │                       │    │                                  │ │
│  │  MCP Protocol:        │    │  标准 JSON HTTP:                 │ │
│  │  - tools/list         │    │  Request:  {"host","command"...} │ │
│  │  - tools/call         │    │  Response: {"status","output"}   │ │
│  │  - initialize         │    │                                  │ │
│  └───────┬───────────────┘    └──────────┬───────────────────────┘ │
│          │                               │                         │
│          └───────────┬───────────────────┘                         │
│                      ▼                                             │
│          ┌───────────────────────┐                                 │
│          │   run_ssh() 核心函数   │                                │
│          │   (Paramiko SSH 引擎)  │                                │
│          └───────────┬───────────┘                                 │
│                      ▼                                             │
│          ┌───────────────────────┐                                 │
│          │  Paramiko SSH Client  │                                 │
│          │  支持密码 / 密钥认证   │                                │
│          └───────────┬───────────┘                                 │
│                      │                                             │
└──────────────────────┼─────────────────────────────────────────────┘
                       ▼
              ┌──────────────────┐
              │  远程 Linux 服务器  │
              │  / Docker 沙箱    │
              └──────────────────┘
```

### 1.2 双通道设计

本服务器同时暴露了**两种接入通道**，这是基于 ESP-Claw 固件现状做出的务实设计决策：

| 通道 | 路径 | 协议 | ESP-Claw 调用方式 | 当前状态 |
| :--- | :--- | :--- | :--- | :--- |
| **MCP SSE** | `GET /sse` | MCP over SSE (Server-Sent Events) | `cap_mcp_client` → `mcp_call_tool` | ⚠️ 受固件 Bug 影响暂不可用 ([Issue #129](https://github.com/espressif/esp-claw/issues/129)) |
| **HTTP REST** | `POST /api/ssh` | 标准 HTTP JSON | `cap_http_request` → POST | ✅ 完全可用，已验证 |

**为什么要保留 MCP SSE 通道？** 虽然目前 ESP-Claw 的 `cap_mcp_client` 存在已知 Bug，但这是官方标准协议通道。一旦固件修复后，无需改动服务器任何代码即可无缝切换到原生 MCP 协议，获得工具自动发现、类型约束校验等高级功能。

### 1.3 核心技术栈

| 组件 | 库 / 框架 | 版本 | 用途 |
| :--- | :--- | :--- | :--- |
| MCP 协议服务端 | `mcp` (官方 Python SDK) | 2.0.0 | MCP Server 实现，SSE 传输层 |
| SSH 客户端引擎 | `paramiko` | 5.0.0 | SSH 协议连接与命令执行 |
| ASGI Web 框架 | `starlette` (内嵌于 MCP SDK) | 1.6.0 | HTTP 路由与请求处理 |
| ASGI 服务器 | `uvicorn` | 0.52.1 | 高性能异步 HTTP 服务器 |
| Python 运行时 | Python | 3.14.6 | 语言运行环境 |

---

## 二、 开发过程与踩坑心得

### 2.1 MCP SDK 版本迁移：`FastMCP` → `MCPServer`

**问题**：最初参考 MCP 官方文档使用 `from mcp.server.fastmcp import FastMCP` 的高级 API，但 `mcp 2.0.0` 版本已经将该接口重构为 `from mcp.server.mcpserver import MCPServer`。

**排查过程**：
```python
# ❌ 旧 API (1.x 版本)
from mcp.server.fastmcp import FastMCP
mcp = FastMCP("server-name")

# ✅ 新 API (2.0.0 版本)
from mcp.server.mcpserver import MCPServer
mcp = MCPServer("server-name")
```

**心得**：MCP SDK 正处于快速迭代期，使用前先通过 `dir()` 和 `help()` 检查实际可用的类和方法。

### 2.2 DNS 重绑定防护导致局域网 IP 访问被拒

**问题**：服务器在 `localhost` 上测试完全正常，但当 ESP32 从局域网 IP (`192.168.3.85:8088`) 发起请求时，服务器返回 **HTTP 421 Misdirected Request**，日志报 `Invalid Host header: 192.168.3.85:8088`。

**根因**：MCP SDK 2.0.0 默认启用了 `TransportSecuritySettings` 中的 DNS 重绑定防护（`enable_dns_rebinding_protection=True`），其 `allowed_hosts` 白名单默认只包含 `localhost` / `127.0.0.1` / `::1`。来自局域网 IP 的 Host Header 被判定为非法。

**修复方案**：
```python
from mcp.server.transport_security import TransportSecuritySettings

sec = TransportSecuritySettings(enable_dns_rebinding_protection=False)
app = mcp.sse_app(transport_security=sec)
```

**心得**：嵌入式设备场景下，客户端永远不会用 `localhost` 访问服务器。在私有局域网内部署 MCP Server 时，必须显式关闭 DNS 重绑定防护或添加所有可能的局域网 IP 到白名单。

### 2.3 SSE App 路由挂载方式的坑

**问题**：最初尝试用 `Starlette(routes=[Mount("/sse", app=sse_app), ...])` 方式组合多个子应用，导致 `/sse` 端点返回 404。

**根因**：`mcp.sse_app()` 返回的 Starlette App 内部已经注册了 `/sse` 和 `/messages/` 路由。当你再用 `Mount("/sse", app=sse_app)` 包装时，实际路径变成了 `/sse/sse`（双重前缀）。

**修复方案**：直接将 `sse_app` 作为顶层 App，额外的自定义路由通过 `app.add_route()` 追加：
```python
app = mcp.sse_app(transport_security=sec)
app.add_route("/api/ssh", http_ssh_api, methods=["POST"])
```

**心得**：理解 MCP SDK 内部的路由结构是关键。`sse_app()` 自身已经是一个完整的 Starlette 应用，直接在其上追加路由比嵌套 Mount 更简洁可靠。

### 2.4 ESP-Claw `cap_mcp_client` 固件 Bug 的发现

**问题**：服务器端一切正常（本地 MCP Client Python 测试完美通过），但 ESP32 设备端所有 MCP 调用都返回 `ESP_ERR_INVALID_STATE`。

**排查过程**：
1. 首先怀疑是服务器端 Host Header 校验问题（已修复，见 2.2）。
2. 修复后 ESP32 仍然报错，深入阅读 `esp-claw` C 源码：
   - `cap_mcp_client_core.c` 中 `cap_mcp_mgr_create()` 调用 `esp_mcp_mgr_init()` → `esp_mcp_mgr_start()` → `esp_mcp_mgr_register_endpoint()` 链路。
   - 底层 `esp_mcp_mgr.c` 在 `esp_mcp_mgr_post_req()` (line 950) 处检查 `MCP instance not configured` 并返回 `ESP_ERR_INVALID_STATE`。
3. 在 GitHub Issue 跟踪器中找到了完全吻合的 Bug 报告：[Issue #129](https://github.com/espressif/esp-claw/issues/129)（Open 状态，未修复）。

**应对策略**：设计了 HTTP REST API 备用通道 (`/api/ssh`)，使用 ESP-Claw 中成熟稳定的 `cap_http_request` 组件绕过 MCP 客户端的 Bug。

**心得**：面向嵌入式设备开发 MCP 服务时，必须充分考虑"设备端固件缺陷"的可能性。设计双通道架构（协议标准通道 + HTTP 降级通道）是保障可用性的关键策略。

### 2.5 SSH 核心逻辑的抽取与复用

**设计决策**：将 SSH 连接、认证、命令执行的逻辑抽取为独立的 `run_ssh()` 函数，返回统一的 `dict` 结构体 `{"status": "success/error", "output/message": "..."}` 。MCP Tool 和 HTTP API 两个通道共享同一个核心函数，避免代码重复：

```python
# MCP Tool 通道调用
@mcp.tool()
def execute_ssh_command(...) -> str:
    res = run_ssh(...)
    return res["output"] if res["status"] == "success" else res["message"]

# HTTP REST 通道调用
async def http_ssh_api(request):
    res = run_ssh(...)
    return JSONResponse(res)
```

---

## 三、 测试过程记录

### 3.1 端到端 MCP 协议测试 (Python Client → Server)

使用 MCP 官方 Python Client SDK 进行了完整的协议握手与工具调用测试：

```python
from mcp.client.sse import sse_client
from mcp.client.session import ClientSession

async with sse_client('http://192.168.3.85:8088/sse') as (read, write):
    async with ClientSession(read, write) as session:
        await session.initialize()
        tools = await session.list_tools()
        result = await session.call_tool('execute_local_command', {'command': 'uname -a'})
```

**测试结果**：

| 测试项 | 结果 |
| :--- | :--- |
| MCP SSE 协议握手 (InitializeRequest) | ✅ 成功 |
| 工具列表获取 (tools/list) | ✅ 成功，返回 2 个工具 |
| `ping_host("127.0.0.1")` 工具调用 | ✅ 成功，返回完整 ICMP 统计 |
| `execute_local_command("uname -a")` | ✅ 成功，返回 `Darwin leodeMac-mini.local 25.5.0 arm64` |
| 通过局域网 IP `192.168.3.85` 访问 | ✅ 成功（修复 DNS 重绑定后） |

### 3.2 HTTP REST API 测试

```bash
curl -X POST http://192.168.3.85:8088/api/ssh \
  -H "Content-Type: application/json" \
  -d '{"host":"127.0.0.1","username":"root","command":"uptime"}'
```

**测试结果**：✅ 成功返回 JSON `{"status": "success", "output": "..."}`

### 3.3 ESP-Claw (ESP32) 实机测试

| 通道 | 调用方式 | 结果 |
| :--- | :--- | :--- |
| HTTP REST (`/api/ssh`) | ESP-Claw 通过 `cap_http_request` POST | ✅ **完全成功** |
| MCP SSE (`/sse`) | ESP-Claw 通过 `cap_mcp_client` `mcp_list_tools` | ❌ 设备端 Bug `ESP_ERR_INVALID_STATE` ([#129](https://github.com/espressif/esp-claw/issues/129)) |

---

## 四、 安装与部署

### 4.1 环境要求

* Python >= 3.10
* macOS / Linux / Windows
* 与 ESP-Claw 设备处于同一局域网

### 4.2 安装步骤

```bash
# 1. 进入项目目录
cd /Users/leoshan/ai_coding/esp-project/mcp_ssh_server

# 2. 创建 Python 虚拟环境（已创建可跳过）
python3 -m venv venv

# 3. 安装依赖
./venv/bin/pip install "mcp[cli]" paramiko uvicorn
```

### 4.3 启动服务器

```bash
cd /Users/leoshan/ai_coding/esp-project/mcp_ssh_server
./venv/bin/python server.py
```

启动后输出：
```
🚀 ESP-Claw SSH Gateway Dual-Mode Server starting on port 8088
   - MCP SSE Endpoint: http://0.0.0.0:8088/sse
   - HTTP REST API Endpoint: POST http://0.0.0.0:8088/api/ssh
INFO:     Uvicorn running on http://0.0.0.0:8088
```

### 4.4 自定义端口

```bash
PORT=9090 ./venv/bin/python server.py
```

### 4.5 后台运行（生产环境）

```bash
nohup ./venv/bin/python server.py > server.log 2>&1 &
```

---

## 五、 使用方法

### 5.1 在 ESP-Claw 中使用（HTTP REST 方式 — 推荐）

在 ESP-Claw 的对话框中输入如下指令：

> "使用 cap_http_request 向 `http://192.168.3.85:8088/api/ssh` 发送 POST 请求，JSON 消息体为：`{"host": "10.0.0.5", "username": "ubuntu", "password": "mypassword", "command": "df -h"}`，告诉我返回结果。"

### 5.2 HTTP REST API 接口文档

**请求**：
```http
POST /api/ssh
Content-Type: application/json

{
  "host": "10.0.0.5",          // 必填：目标服务器 IP
  "command": "df -h",           // 必填：要执行的 Shell 命令
  "username": "root",           // 可选：SSH 用户名，默认 root
  "password": "123456",         // 可选：SSH 密码
  "port": 22,                   // 可选：SSH 端口，默认 22
  "key_path": "~/.ssh/id_rsa"  // 可选：私钥文件路径
}
```

**响应（成功）**：
```json
{
  "status": "success",
  "output": "Filesystem      Size  Used Avail Use% Mounted on\n/dev/sda1        50G   12G   36G  25% /"
}
```

**响应（失败）**：
```json
{
  "status": "error",
  "message": "[SSH Error]: AuthenticationException - Authentication failed."
}
```

### 5.3 MCP 工具列表（MCP SSE 通道）

通过 MCP 协议自动暴露以下工具：

| 工具名 | 功能 | 输入参数 |
| :--- | :--- | :--- |
| `execute_ssh_command` | 通过 SSH 在远程服务器上执行 Shell 命令 | `host`*, `command`*, `username`, `password`, `port`, `key_path` |
| `execute_local_command` | 在网关本机执行 Bash 命令 | `command`* |

> `*` 标注为必填参数

---

## 六、 项目文件结构

```
mcp_ssh_server/
├── server.py          # 主服务器程序（MCP SSE + HTTP REST 双通道）
├── test_server.py     # 早期测试版本（已废弃，仅保留参考）
├── ARCHITECTURE.md    # 本文档
└── venv/              # Python 虚拟环境（已安装 mcp, paramiko, uvicorn）
```

---

## 七、 未来演进方向

1. **固件 Bug 修复后切换到原生 MCP 通道**：一旦 `esp-claw` 官方修复 Issue #129，无需改动服务器代码，ESP-Claw 即可通过 MCP SSE 原生调用 `execute_ssh_command` 工具。
2. **增加 SSH 密钥集中管理**：可扩展为从加密配置文件或 Vault 中读取 SSH 凭据，避免明文传输。
3. **增加 Docker Engine API 工具**：可新增 `@mcp.tool()` 封装 Docker Remote API，实现容器生命周期管理。
4. **增加命令审计日志**：记录所有通过网关执行的 SSH 命令，用于安全审计和回溯。
