# 语音交互管线设计文档（cap_voice_pipeline）

> 分支：`feature/voice-asr-tts`（基线 tag `baseline-before-voice-port`）
> 目标硬件：ESP32-S3-DevKitC-1（16 MB Flash / 8 MB PSRAM）+ INMP441 麦克风 + MAX98357A 功放 + 0.91" SSD1306 OLED
> 云端服务：火山引擎豆包语音（大模型流式识别 + 单向流式合成）

---

## 1. 背景与目标

ESP-Claw 原生的交互入口是 IM（微信/飞书/Telegram/QQ）和网页聊天，没有语音。本分支把
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的音频接线方案移植过来，但**不搬它的代码结构**——
xiaozhi 是围绕单一状态机写死的语音固件，而 ESP-Claw 是能力（capability）+ 事件路由的 Agent 运行时，
两者的事件模型不兼容。

因此语音在本设计中被定位为**一个普通的输入输出通道**，与微信、飞书平级：

- 识别出的文本作为一条普通用户消息投进 `claw_event_router`
- Agent 的最终回复经 `cap_im_local` 的出站观察者转成语音播报

这样语音天然复用了 Agent 已有的一切：LLM 推理、工具调用、Lua 动态脚本、端侧记忆、联网搜索。

### 最终形态

```
按住 BOOT 键说话  →  流式识别  →  Agent 思考 / 联网 / 调工具  →  语音播报答案
                                        ↕
                              OLED 显示当前状态
```

---

## 2. 整体架构

```mermaid
graph TB
    subgraph HW["硬件层"]
        MIC["INMP441 麦克风<br/>I2S0 · 16 kHz"]
        SPK["MAX98357A 功放<br/>I2S1 · 24 kHz"]
        OLED["SSD1306 OLED<br/>I2C · 128×32"]
        BTN["BOOT 按键<br/>GPIO 0"]
    end

    subgraph BM["esp_board_manager"]
        P1["i2s_audio_in"]
        P2["i2s_audio_out"]
        P3["i2c_master"]
    end

    subgraph CAP["cap_voice_pipeline"]
        AIO["voice_audio_io<br/>借用板卡 I2S 句柄"]
        ASR["volcengine_asr<br/>WebSocket 二进制协议"]
        TTS["volcengine_tts<br/>HTTP chunked + 预缓冲"]
        DISP["voice_status_display<br/>5×7 字库 · 状态标签"]
        CORE["cap_voice_pipeline<br/>状态机 · 任务编排"]
    end

    subgraph CLAW["ESP-Claw 运行时"]
        ROUTER["claw_event_router"]
        AGENT["claw_core<br/>Agent Loop"]
        IMLOCAL["cap_im_local"]
        CAPREG["claw_cap 能力注册表"]
    end

    subgraph CLOUD["火山引擎"]
        VASR["大模型流式语音识别<br/>sauc/bigmodel_nostream"]
        VTTS["单向流式语音合成<br/>tts/unidirectional"]
    end

    MIC --> P1 --> AIO
    SPK --> P2 --> AIO
    OLED --> P3 --> DISP
    BTN -.Lua 脚本.-> CORE

    AIO --> ASR
    AIO --> TTS
    CORE --> ASR
    CORE --> TTS
    CORE --> DISP
    CORE --> CAPREG

    ASR <-->|"wss"| VASR
    TTS <-->|"https"| VTTS

    ASR -->|"识别文本"| ROUTER
    ROUTER --> AGENT
    AGENT -->|"out_message"| IMLOCAL
    IMLOCAL -->|"出站观察者"| CORE
```

### 模块职责

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `cap_voice_pipeline.c` | 755 | 状态机、任务编排、capability 注册、TTS 队列 |
| `volcengine_asr.c` | 755 | 流式识别客户端，WebSocket 二进制分帧 |
| `volcengine_tts.c` | 770 | 流式合成客户端，chunked JSON 切分、base64、生产者/消费者播放 |
| `voice_status_display.c` | 487 | SSD1306 驱动 + 5×7 字库 + 状态标签 |
| `voice_audio_io.c` | 168 | 向板卡管理器借用 I2S 通道，校验采样率 |
| `voice_pipeline_private.h` | 109 | 组件私有接口 |

---

## 3. 功能特性清单

本分支共 46 个提交。按功能域归类如下（✅ 已实测通过，⚠️ 有已知限制，🚧 已实现但未经硬件验证）。

### 3.1 语音识别（ASR）

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| 火山引擎 v3 流式识别集成 | ✅ | `wss://.../api/v3/sauc/bigmodel_nostream`，替代初版的 v1 REST 接口 |
| 自定义二进制协议分帧 | ✅ | 大端序，header/sequence/payload-size 三段，四种消息类型 |
| 末包负序号标记 | ✅ | 双缓冲始终留一块音频，确保末包携带真实音频而非空包 |
| **独立采集任务 + 环形缓冲** | ✅ | 麦克风由专属任务持续读入 4 秒 PSRAM 环，网络再慢也不丢音频。取代了原先只覆盖握手期的预缓冲 |
| 握手与采集并行 | ✅ | 抢救回握手期间约 800 ms 音频，按下即说不再丢开头 |
| **抬起后排空积压** | ✅ | 松手只停麦克风，已采集未发送的音频继续送完，不再切掉每句最后一个词 |
| 采集时长下限保护 | ✅ | <600 ms 判定为误触，不发末包、不等转写 |
| 错误帧正确解析 | ✅ | `header \| error_code \| message_size \| message`，鉴权失败可诊断 |
| 服务端响应日志 | ✅ | 完整打印 JSON，便于定位服务端问题 |
| 免按键 VAD 断句 | ⚠️ 未做 | 目前靠按住说话界定句子边界 |

### 3.2 语音合成（TTS）

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| 火山引擎 v3 单向流式合成 | ✅ | `POST /api/v3/tts/unidirectional`，HTTP chunked |
| chunked JSON 增量切分 | ✅ | 括号配平扫描，跳过字符串内的括号与转义 |
| base64 解码 + PCM 直推 | ✅ | 不需要 MP3 解码器，采样率取板卡实际配置 |
| **预缓冲流式播放** | ✅ | 攒够 2 秒即开播，合成等待 12.5 s → 2.3 s |
| 生产者/消费者解耦 | ✅ | 播放在独立任务，避免阻塞 HTTP 回调导致 DMA 饿死 |
| **I2S 分块写入** | ✅ | 8 KB 一块，修复整段写入必然超时的缺陷 |
| **Markdown 过滤** | ✅ | 设备侧清理 `**` / `` ` `` / `#` / 列表 / `[文字](链接)`，避免念成"星星你好星星" |
| **软件音量控制** | ✅ | Q15 定点衰减，增益取 `(p/100)²`；本板功放无增益接口，这是唯一可行的音量控制 |
| 播放互斥 | ✅ | Agent 回复与 `voice_speak` 工具调用不会在 I2S 上串音 |
| TTS 任务队列 | ✅ | 回调只入队，不阻塞消息投递任务（原会阻塞最长 20 s） |
| 成功码兼容 | ✅ | 同时接受 `0` 与线上实际返回的 `20000000` |

### 3.3 音频硬件层

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| 板卡管理器音频接入 | ✅ | `esp_board_periph_ref_handle()` 借用 I2S 通道，capability 不碰 GPIO |
| 采样率运行时校验 | ✅ | 从板卡配置读取，麦克风非 16 kHz 直接报错退出 |
| 引脚冲突预检 | ✅ | `esp_gpio_is_reserved()` 预检并点名冲突引脚（IDF 的 I2S 只警告不报错） |
| **Dummy codec 启动修复** | ✅ | `CONFIG_CODEC_DUMMY_SUPPORT=y`，消除 `audio_dac` 每次启动的三行报错 |
| 引用计数正确性 | ✅ | `ref_handle` / `unref_handle` 配对，避免板卡 deinit 后留下悬空句柄 |
| **I2S 通道自愈** | ✅ | 被其他组件 `i2s_channel_disable()` 关掉后，重新套用板卡配置并使能，不必重启 |

### 3.4 触发方式

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| **Push-to-talk（按住说话）** | ✅ | 板载 BOOT 键 GPIO 0，无需接线；松手即停，绕开 VAD |
| **外接按钮** | ✅ | GPIO 21，两根线到 GND，驱动自动开内部上拉。引脚列表由 router 规则传入，换脚不用重编译 |
| **BLE 手机触发** | ✅ | 自定义 GATT 服务，写 1 字节 `01`/`00`/`02`。任意 BLE 调试助手可用，等于"手机当按钮" |
| **BLE HID 实体按钮** | 🚧 | 扫描 `0x1812` 并连接，输入报文非全零判为按住。**未经硬件验证** |
| **IM `/listen` 指令** | ✅ | 飞书/微信等任意已绑定通道，router 规则实现，无需改 C 代码 |
| 控制台调用 | ✅ | `cap call voice_listen {}` |
| Agent 自主调用 | ✅ | `voice_listen` / `voice_stop_listening` / `voice_speak` 三个工具 |
| 内置 skill 打包 | ✅ | 按键脚本随固件进只读 SYSTEM 镜像，开机 router 规则常驻拉起 |
| 唤醒词 | ⚠️ 未做 | 需 ESP-SR / WakeNet，模型占 flash、麦克风需常开 |

### 3.5 OLED 状态显示

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| SSD1306 驱动（C 侧） | ✅ | 128×32，页寻址，自建帧缓冲 |
| I2C 地址自动探测 | ✅ | `i2c_master_probe()` 试 0x3C / 0x3D |
| **状态显示** | ✅ | `IDLE` / `LISTEN` / `THINK` / `TALK` / `RETRY?` |
| 5×7 字库 + 缩放渲染 | ✅ | A-Z、0-9、常用标点；状态标签 3 倍、消息 2 倍 |
| 状态转换互斥 | ✅ | 采集任务与播放任务并发写屏不会撕裂 |
| 状态优先级 | ✅ | 播报中不被 `RETRY?` 抢屏；多段回复不闪回 IDLE |
| `display_text` capability | ✅ | Agent 可主动写屏，仅在探测到面板时才注册 |
| 中文显示 | ⚠️ 不支持 | 5×7 字库无中文字形，capability 描述里已声明 |
| 纳入板卡管理器 | ⚠️ 未做 | 目前由本组件直驱，Lua 的 `display`/`lcd` 模块用不了它 |

### 3.6 ESP-Claw 框架集成

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| 识别文本投递事件路由 | ✅ | `claw_event_router_publish_message(channel=local)` |
| **`local` 出站 channel 绑定** | ✅ | 框架原本只绑定了 qq/feishu/telegram/wechat/web，缺 `local` 导致回复被丢弃 |
| **出站观察者机制** | ✅ | 给 `cap_im_local` 新增观察者列表，语音以观察者接入，不再抢占网页聊天的主回调 |
| `event_type` 透传 | ✅ | 只播报 `out_message`，不念"正在处理"和每次工具调用的流水账 |
| **能力对大模型可见** | ✅ | 新增 `claw_cap_add_llm_visible_group()`；原先注册后仍不可见，Agent 完全看不到语音与屏幕工具 |
| 语音回复长度约束 | ✅ | `soul.md` 按 `source_chat_id: voice_user` 限制五句话，不影响 IM 通道 |

### 3.7 板级支持与配置

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| **新增 `esp32_S3_DevKitC_1_voice` 板型** | ✅ | 独立板卡定义，不改动上游原板型 |
| I2S 麦克风/喇叭声明 | ✅ | INMP441（I2S0/16 kHz/左声道）+ MAX98357A（I2S1/24 kHz） |
| I2C OLED 声明 | ✅ | GPIO 41/42 |
| **16 MB Flash 启用** | ✅ | 原为 8 MB，半片 flash 闲置；应用分区 0x3b6000 → 5 MB |
| Kconfig 开关真实生效 | ✅ | 关闭后语音代码完全不编译（约省 10 KB） |
| `CONFIG_WS_BUFFER_SIZE=4096` | ✅ | 默认 1024 装不下 ASR 网关的握手响应头 |

### 3.8 Web 配置界面

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| **语音设置页面** | ✅ | 侧边栏「语音服务 (ASR/TTS)」，SolidJS |
| 单 API Key 模型 | ✅ | 新版控制台只需一个 `X-Api-Key`，识别与合成共用 |
| 一键填充默认端点 | ✅ | 页头按钮，仅 API Key 需手填 |
| 前端类型检查通过 | ✅ | 初版有 37 个类型错误导致页面白屏（`pnpm build` 不做类型检查） |
| NVS 键名隔离 | ✅ | v3 键名加后缀，避免已配置设备沿用 v1 的 `https://` 地址喂给 WebSocket |

### 3.9 构建与依赖

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| **CMake 依赖修复** | ✅ | 组件 `REQUIRES` 恒定声明（IDF 两次解析，条件声明会静默丢依赖） |
| `EXTRA_COMPONENT_DIRS` 补齐 | ✅ | 加入 `claw_capabilities` 搜索路径 |
| 第三方驱动适配 ESP-IDF 6.0 | ✅ | `esp_lcd_gc9107` / `esp_lcd_gc9d01` 的 `rgb_ele_order` 枚举、`ir_learn` 的 `esp_driver_rmt` 依赖 |
| **改动组件迁入工程目录** | ✅ | 上述三个组件从 `managed_components/` 迁至 `application/edge_agent/components/` 并纳入版本控制——包管理器对改动过的托管组件做哈希校验，会拦截 `idf.py fullclean` |
| `lua_module_lcd` v6 兼容 | ✅ | `esp_lcd_nt35510` 头文件条件宏保护 |

> **说明**：上述三个第三方驱动**并非新增依赖**，而是原本就由包管理器下载的托管组件（`managed_components/` 在 .gitignore 中）。
> 因为需要改造以适配 ESP-IDF 6.0，按官方规范迁入工程自有目录并提交，避免哈希校验冲突。

### 3.10 蓝牙无线触发（`cap_ble_trigger`，约 1560 行）

独立组件，与语音管线解耦：两路输入都汇到 `ble_trigger_report()`，再由工作任务调
`voice_listen` / `voice_stop_listening`。

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| **触发 GATT 服务（外围角色）** | ✅ | 板子广播 `esp-claw-mic`，手机连入写 1 字节。**不加密标志，无需配对** |
| 按住 / 单击两种语义 | ✅ | `01`/`00` 按住说话；`02` 单击切换，供只能发单次事件的客户端 |
| **BLE HID 主机（中心角色）** | 🚧 | 扫描 `0x1812`、Just Works 绑定、密钥存 NVS |
| 通用输入报文映射 | 🚧 | 不解析键值：非全零=按住，全零=松开。适配未知按钮 |
| 回调解耦 | ✅ | GATT 写发生在 NimBLE 主机任务上，改为入队后由工作任务执行，不阻塞链路层 |
| 边沿去重 | ✅ | HID 按键重复每秒几十次同样报文，只有状态跳变才入队 |
| 未松开保护 | ✅ | 15 秒无松开自动停止，防止断连后麦克风常开 |
| 扫描诊断 | ✅ | `ble_trigger_scan` 列出所有扫到的设备（MAC/RSSI/是否 HID），能区分"按钮不兼容"和"不在范围" |
| 配对应答 | ✅ | 处理 `PASSKEY_ACTION` 与 `REPEAT_PAIRING`；无屏幕时把配对码打到串口 |
| Wi-Fi 共存 | ✅ | 扫描为被动、50% 占空、仅按需触发；开机扫描延后 6 秒避开 Wi-Fi 关联 |
| 自动重连实体按钮 | ⚠️ 未做 | 重启后按钮多在休眠不广播，需手动 `ble_trigger_scan` |
| 指定连接目标 | ⚠️ 未做 | 连扫到的第一个 HID 设备，附近有键鼠可能连错 |

---

## 4. 云端方案选型

### 4.1 ASR：为什么选流式识别而不是录音文件识别

火山引擎提供两类语音识别，形态完全不同：

| | **大模型流式识别（选用）** | 录音文件识别（否决） |
| --- | --- | --- |
| 接口 | `wss://.../api/v3/sauc/bigmodel_nostream` | `POST /api/v3/auc/bigmodel/submit` + 轮询 `query` |
| 音频怎么给 | 设备直接推 PCM 字节流 | **`audio.url` 必填**，只接受音频链接 |
| 额外依赖 | 无 | **需要一个公网可访问的对象存储** |
| 交互次数 | 1 条 WebSocket | 上传 + submit + 轮询 N 次 |
| 延迟 | 5 s 音频约 300~400 ms 出结果 | 上传 + 排队 + 轮询，秒级起 |
| 定位 | 实时语音交互 | 离线批量转写（限制 <512 MB） |

**决定性因素**：录音文件识别的 `audio.url` 是必填项，没有任何直接 POST 音频字节的途径。
ESP32 得先把录音传到公网可达的地方——而设备的 HTTP 服务在局域网内（`192.168.3.x`），
火山的云端根本连不进来。"HTTP 比 WebSocket 简单"这个直觉在这里是反的：
HTTP 那条路把复杂度转移到了"你得先有个公网文件服务"，工作量远大于实现二进制分帧。

流式识别又分三种端点，选了 **`bigmodel_nostream`（流式输入模式）**：

- `bigmodel`：双向流式，边说边出字，但 Agent 只消费完整句子，中间结果会被丢弃
- `bigmodel_async`：双向流式优化版，同上
- **`bigmodel_nostream`：准确率更高**，末包后返回最终结果 —— 正是我们要的

### 4.2 TTS：为什么选 HTTP 单向流式

三种合成接口能力等价，只是传输方式不同：

| 接口 | 传输 | 评价 |
| --- | --- | --- |
| **单向流式 HTTP（选用）** | HTTP chunked | 复用现有 `esp_http_client`，零新增协议栈 |
| 单向流式 WebSocket | WebSocket | 需额外实现二进制分帧，收益为零 |
| 双向流式 WebSocket | WebSocket | TTS 是"一次文本进、音频流出"，用不上双向能力 |

且文档明确写"流式场景推荐使用 pcm"，而 **PCM 正是 I2S 能直接播的格式**——不需要 MP3 解码器。
合成采样率直接取板卡喇叭通道的实际配置（24 kHz），固件端零重采样。

### 4.3 鉴权

新版控制台只需**一个 `X-Api-Key`**，识别与合成共用。旧版控制台的
`X-Api-App-Key` + `X-Api-Access-Key` 组合已不需要。

| 服务 | Header |
| --- | --- |
| ASR | `X-Api-Key` / `X-Api-Resource-Id: volc.seedasr.sauc.duration` / `X-Api-Request-Id` / `X-Api-Sequence: -1` |
| TTS | `X-Api-Key` / `X-Api-Resource-Id: seed-tts-2.0` / `X-Api-Request-Id` |

---

## 5. 端侧实现

### 5.1 完整交互时序

```mermaid
sequenceDiagram
    participant U as 用户
    participant BTN as BOOT 键(Lua)
    participant VP as cap_voice_pipeline
    participant ASR as volcengine_asr
    participant VA as 火山 ASR
    participant R as claw_event_router
    participant AG as claw_core Agent
    participant IM as cap_im_local
    participant TTS as volcengine_tts
    participant VT as 火山 TTS
    participant SPK as 喇叭

    U->>BTN: 按下
    BTN->>VP: voice_listen
    VP->>VP: 屏幕 → LISTEN
    par 并行（关键设计）
        ASR->>VA: WebSocket 握手
    and
        ASR->>ASR: 采集进预缓冲
    end
    ASR->>VA: full client request(JSON 音频参数)
    ASR->>VA: 补发握手期间的音频
    loop 每 200 ms
        ASR->>VA: audio only(正序号)
    end
    U->>BTN: 松开
    BTN->>VP: voice_stop_listening
    ASR->>VA: audio only(负序号) 末包
    VA-->>ASR: full server response(最终文本)
    ASR->>VP: 识别文本
    VP->>VP: 屏幕 → THINK
    VP->>R: publish_message(channel=local)
    R->>AG: 触发 Agent Loop
    AG->>AG: 工具调用 / 联网搜索
    AG->>R: out_message
    R->>IM: send_message(local_send_message)
    IM->>VP: 出站观察者(event_type=out_message)
    VP->>VP: 屏幕 → TALK
    VP->>TTS: 入队播报
    TTS->>VT: POST 合成请求
    VT-->>TTS: chunked JSON 切片
    TTS->>SPK: 攒够 2 秒后边收边播
    VP->>VP: 屏幕 → IDLE
```

### 5.2 ASR 二进制协议

火山流式识别用自定义二进制帧（整数**大端**）：

```
byte 0: 协议版本(4b) | 头长度(4b，单位=4字节)
byte 1: 消息类型(4b) | 类型标志(4b)
byte 2: 序列化方式(4b) | 压缩方式(4b)
byte 3: 保留
[4 字节 sequence，当标志位声明时]
4 字节 payload 长度
payload
```

| 消息类型 | 值 | 用途 |
| --- | --- | --- |
| full client request | `0b0001` | 首包，JSON 声明音频格式 |
| audio only request | `0b0010` | 音频数据包 |
| full server response | `0b1001` | 识别结果 |
| error | `0b1111` | 服务端错误 |

**末包用负序号标记**（标志 `0b0011`），这是"请给我最终结果"的信号。实现上用**双缓冲**
始终留一块音频不发，等确认没有下一块了，才把它作为负包发出去——保证末包携带真实音频而非空包。

### 5.3 TTS 流式播放的生产者/消费者

```mermaid
graph LR
    subgraph HTTP["HTTP 回调（生产者）"]
        A["chunked 数据到达"] --> B["括号配平切分<br/>JSON 对象"]
        B --> C["base64 解码"]
        C --> D["追加进 PSRAM 缓冲"]
    end

    subgraph BUF["共享缓冲"]
        E["pcm[] · 互斥锁保护<br/>pcm_len / played"]
    end

    subgraph PLAY["播放任务（消费者）"]
        F["锁内拷贝到暂存区"] --> G["锁外写 I2S<br/>8 KB 一块"]
    end

    D --> E
    E --> F
    D -.攒够 2 秒.-> H["启动播放任务"]
    H --> F
```

三个关键点：

1. **播放必须在独立任务里**。若在 HTTP 回调里直接播，`i2s_channel_write` 会阻塞回调，
   反过来节流 TCP 接收，DMA 饿死 → 爆音。
2. **消费者在锁内拷贝、锁外写**。生产者扩容缓冲时会 `realloc`，消费者若跨越扩容持有旧指针
   就是 use-after-free。
3. **扩容的大块拷贝必须在锁外做**。上层翻倍时是几 MB 的 PSRAM 拷贝，持锁会把播放任务饿死。

### 5.4 状态显示

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> LISTEN: voice_listen
    LISTEN --> THINK: 识别成功，投给 Agent
    LISTEN --> RETRY: 采集 <600 ms 或识别失败
    THINK --> TALK: 收到 out_message
    TALK --> IDLE: 播放完且队列为空
    RETRY --> LISTEN: 下次按键
    note right of RETRY
        不阻塞、不自动回 IDLE
        播报中则跳过（不抢屏）
    end note
```

128×32 单色屏画不了有意义的表情（实测用户反馈"这个看着有点抽象啊"），改用
**3 倍放大的 5×7 字库**显示 `IDLE` / `LISTEN` / `THINK` / `TALK` / `RETRY?`。

### 5.5 触发方式

| 方式 | 实现 | 说明 |
| --- | --- | --- |
| **按住说话** | `push_to_talk.lua` + `lua_module_button` | BOOT 键（GPIO 0）与外接按钮（GPIO 21），按下 `voice_listen`、松开 `voice_stop_listening`。**松手即停，绕开了做 VAD 的工程量** |
| **BLE 无线触发** | `cap_ble_trigger` | 手机写 GATT 特征，或实体 BLE HID 按钮。详见第 8 节 |
| IM 指令 | router 规则 `/listen` 前缀 | 飞书/微信等任意已绑定通道 |
| 控制台 | `cap call voice_listen {}` | 调试用 |
| Agent 自主 | LLM 工具调用 | `voice_listen` / `voice_speak` / `display_text` |

按键脚本作为**固件内置 skill** 放在组件的 `skills/` 目录，构建时进只读 SYSTEM 镜像
（`/system/skills/voice_push_to_talk/`），由开机 router 规则以 `lua_run_script_async` 常驻拉起。

---

## 6. 踩坑记录

> 这一节按类别整理开发过程中实际遇到的问题。多数不是"写错一行"，而是**假设与现实不符**。

### 6.1 构建系统

**① 组件 CMakeLists 的 `REQUIRES` 不能用 `if(CONFIG_xxx)` 包起来**

ESP-IDF 会**两次解析**组件 CMakeLists，第一次（依赖展开阶段）`CONFIG_*` 尚未定义，
`else()` 分支生效导致依赖全部丢失，报 `cJSON.h: No such file`。

正解：`REQUIRES` 恒定声明，用源码内 `#if CONFIG_xxx` 做条件编译。

**② IDF v6 中 `json` 组件已不存在**，cJSON 由 managed component `espressif__cjson` 传递引入。

**③ 修改过的 `managed_components` 会阻塞 `idf.py fullclean`**

包管理器做哈希校验，检测到文件被改会拦截。正解：把改过的组件迁到工程自有的 `components/` 目录。

### 6.2 硬件与板卡

**④ `slot_mask` 抄错导致采集速率翻倍**

从别的板卡抄了 `slot_mode: MONO` + `slot_mask: I2S_STD_SLOT_BOTH`。MONO 模式下用 BOTH
会**同时采集两个 slot**，字节率翻倍、数据变成交错流，服务端拿到无法解码的内容后直接关连接，
**一个结果帧都不回**。

发现方式：日志显示"录了 4000 ms"但墙钟只过了 1997 ms —— **正好 2 倍**。
正解：INMP441 的 L/R 接地，只驱动左声道，应为 `I2S_STD_SLOT_LEFT`（xiaozhi 的
`no_audio_codec.cc:48` 即如此）。

**⑤ 引脚冲突：ESP-IDF 的 I2S 只警告不报错**

```c
// esp_driver_i2s/i2s_common.c:893
if (!used_by_pair_chan && (esp_gpio_reserve(BIT64(gpio_num)) & BIT64(gpio_num))) {
    ESP_LOGW(TAG, "GPIO %d is not usable, maybe conflict with others");
}   // ← 只警告，继续执行，引脚被抢走
```

所有 `if (ret != ESP_OK)` 检查全部通过，初始化"成功"，实际上把 LCD 的 SPI 时钟、MOSI、CS
和背光都抢了。**六个引脚全部撞车而毫无察觉。**

正解有两层：
- 短期：初始化前用 `esp_gpio_is_reserved()` 预检，冲突则明确失败
- 根治：**改用板卡管理器的 `i2s_audio_in` / `i2s_audio_out`**，capability 完全不碰 GPIO，
  冲突在结构上不可能发生

**⑥ `esp_board_periph_get_handle` vs `ref_handle`**

前者不做惰性初始化、不引用计数；后者两者都做。用错了会在板卡之后 deinit 音频设备时留下悬空句柄。

**⑦ 采样率不能靠 Kconfig 猜**

应当用 `esp_board_manager_get_periph_config()` 读板卡真实配置。写死 24 kHz 会在
任何 YAML 不同的板子上悄悄坏掉（如 48 kHz 的板子会以半速播放）。

**⑧ `CONFIG_CODEC_DUMMY_SUPPORT` 默认关闭**

MAX98357A 是没有控制接口的裸功放，板卡管理器用 "dummy" codec 承载它，但该条目
默认不编进注册表，导致每次启动报 `Unsupported codec: dummy` + `Failed to init device: audio_dac`。

### 6.3 协议与云服务

**⑨ `CONFIG_WS_BUFFER_SIZE` 默认 1024 太小**

`transport_ws.c` 用**同一个缓冲区**既拼握手请求、又读握手响应，找不到 `\r\n\r\n` 就放弃。
字节跳动网关的响应头带大量 trace 字段超过 1023 字节，握手直接失败。

**极易踩的坑**：`esp_websocket_client_config_t.buffer_size` 是**另一个**缓冲区（收发数据帧），
设它没用。必须调 `CONFIG_WS_BUFFER_SIZE`。

**⑩ `20000000` 才是成功码，不是 `0`**

TTS 文档的响应示例写 `"code": 0`，但线上实际返回 `20000000`（火山语音接口族共用的错误码表里
`20000000 = 成功`）。照文档示例写 `code != 0 就是错误`，会导致**声音正常播出但返回失败**——
因为报错的是音频之后的收尾包。

**⑪ 错误帧格式与普通帧不同**

ASR 错误帧是 `header | error_code(4) | message_size(4) | message`，把 error_code 当成
payload 长度解析，鉴权失败时打出来的是二进制乱码而非可诊断的错误码。

**⑫ 服务端产出速度的测量会被自身实现污染**

最初测得"TTS 只有实时的 59%"，据此得出"必须全部收完才能播"。但那次测量时播放内联在
HTTP 回调里，`i2s_channel_write` 阻塞回调**反过来节流了 TCP 接收**——测到的是耦合系统的速度。
播放解耦后重测是 **114~122%**，于是可以改成预缓冲 2 秒即开播，合成等待从 12.5 秒降到 2.3 秒。

**教训**：测量前先确认测的是被测对象，而不是自己的实现。

### 6.4 并发与内存

**⑬ ESP-IDF 的 `xTaskCreate` 栈大小单位是字节，不是字**

`task.h:576` 明确写 "the stack size DEFINED IN BYTES"。在 4096 字节的任务里放了
8192 字节的局部数组，**每次播报都会踩栈**。这也正好违反仓库 CLAUDE.md 里"栈变量不超过 128 字节"
的规定——那条规则就是为防这个存在的。

**⑭ join 超时后释放共享状态 = use-after-free**

给播放任务的 join 设了 30 秒超时，超时后直接 `vSemaphoreDelete` + `free(pcm)` + 函数返回。
但 `stream` 是**栈上局部变量**，播放任务还活着，下一步就去 take 已删除的信号量、
memcpy 已释放的内存、写已被复用的栈帧。而 60 秒的音频上限配 30 秒 join 超时，
这条路径**结构性可达**。

正解：播放任务内部所有等待都有界（每块 I2S 写有超时、互斥锁只短暂持有），必然结束，
所以 join 改成无限等待反而是安全的。

**⑮ 单槽位回调被静默覆盖**

`cap_im_local` 的出站回调是单槽位、后写覆盖前写。语音管线初始化时覆盖了网页聊天的
投递回调，导致**网页聊天页从此收不到任何回复且无任何报错**。

正解：给 `cap_im_local` 增加独立的**观察者列表**——主回调仍归网页聊天所有、返回值照常传播，
语音以观察者身份接入，出错只记日志不影响投递。

**⑯ 用后即释放的配置指针**

`main.c` 把 `s_config` 传进来，却在 `app_main` 结尾 `free()` 掉。语音管线一直持有这个指针，
之后每次读 API Key 都是读已释放内存。（`app_claw_start` 是拷贝的，只有这个 capability 是例外。）

### 6.5 集成与可见性

**⑰ 注册 capability ≠ 大模型能看见它**

这是最隐蔽的一个。`app_claw_start()` 在启动时调用 `claw_cap_set_llm_visible_groups()`，
列表来自静态表；而语音 capability 是在 `app_claw_start` **之后**从 `main.c` 注册的，
不在可见列表里。

后果：`cap list` 能列出 `voice_listen` / `display_text`，控制台也能调用，**但大模型完全看不见**。
用户让它"在屏幕上显示文字"，它转了 11 轮、扫了 I2C 总线，最后回答"这个设备没有屏幕"——
它不是判断失误，是真的看不到工具。

正解：给 claw_cap 增加 `claw_cap_add_llm_visible_group()`（原来只有整体替换），
注册后追加。注意**列表为空时必须 no-op**，因为空列表本身就是"全部可见"的意思，
这时追加一项反而会隐藏其他所有 group。

**⑱ 出站投递需要 channel 绑定**

`app_claw` 只绑定了 `qq`/`feishu`/`telegram`/`wechat`/`web`，唯独没有 `local`——
而这恰恰是 `cap_im_local` 自己的默认 channel。Agent 的回复因此被丢弃，
日志是 `send_message resolve failed channel=local`。

**⑲ 绑定后要区分消息类型，否则喇叭念流水账**

三条 router 规则都走同一条 `send_message` 路径：inbound ack（🦞 正在处理）、
每次工具调用的 `agent_stage` 通知、最终答案 `out_message`。全部播报的话，
**答案会被淹没在流水账里**。

好在路由器的 payload 里已经带了 `event_type`，只是 `cap_im_local` 把它丢掉了——
接出来后过滤 `out_message` 即可。

### 6.6 延迟与体验

**⑳ 先建连再采集 = 丢掉开头半秒语音**

握手实测 540~602 ms，而 I2S DMA 只保留约 90 ms（`dma_desc_num=6 × dma_frame_num=240`
@16 kHz）。建连期间麦克风在跑但无人读取，**约 450~510 ms 的语音被环形缓冲覆盖丢弃**。
按下就说话会缺字。

正解：`esp_websocket_client_start()` **本来就是非阻塞的**，是 `xEventGroupWaitBits`
把它变成了阻塞等待。改成边采集边轮询连接状态，连上后把预缓冲一次性补发。

**㉑ 一次性写入整段音频会超时**

`i2s_channel_write` 要等这批数据**全部排进 DMA 才返回**，排完 19 秒音频就得 19 秒。
配 2000 ms 超时必然返回 `ESP_ERR_TIMEOUT`，被当致命错误后**一点声音都没有**。
改缓冲前该函数是按小片调用的，2000 ms 绰绰有余——**改了调用方式却没同步改参数假设**。

**㉒ 控制台 JSON 需要转义**

`esp_console` 的分词器会吃掉双引号、按空格切分。`cap call voice_speak {"text":"hi"}`
会被处理成 `{text:hi}`。正确写法：`cap call voice_speak "{\"text\":\"hi\"}"`。

**㉓ TTS 默认会把 markdown 念出来**

火山文档原话：`**你好**` 朗读为 **"星星你好星星"**。服务端有 `disable_markdown_filter`
参数，但文档把 `additions` 的类型标成 `string` 却列了对象字段、且无示例，格式吃不准，
**发错可能整个请求被拒**。改在设备侧清理更确定，还能兜住"模型没听提示词照样输出 markdown"。

清理时注意：**删除线是双波浪 `~~`，单个 `~` 不能删**——中文里 `26~34` 是温度区间，
误删会把"26 到 34 度"变成"2634 度"。

**㉔ 前端 `pnpm build` 不做类型检查**

Voice 配置页调用 `createConfigTab('voice', [...])`（两个位置参数），而该函数签名是
接收单个 options 对象。`options.toForm` 是 undefined → 组件 setup 阶段抛
`TypeError` → 懒加载路由白屏。`pnpm typecheck` 一跑，这一个文件报 **37 个错误**——
说明它从未通过类型检查就被打包发布了。

**㉕ NVS 键名复用会让已配置的设备静默失效**

v1 → v3 迁移时若沿用 `asr_base_url` 等键名，**已存的值优先于新默认值**，
设备会拿 v1 的 `https://` 地址去喂 WebSocket 客户端（scheme 不对直接被拒）。
正解：新键名加 `v3` 后缀，让旧值自然被忽略。

### 6.7 音频通道所有权

**㉖ 这块板子根本没有音量控制，而"调音量"会把喇叭弄哑**

Agent 被要求调音量时，会去用 Lua 的 `audio` 模块 `output:set_volume(50)`。两个问题叠加：

1. **调了也没用**：MAX98357A 是定增益功放没有控制接口，板卡 YAML 用 `chip: internal`
   → dummy codec，其 `set_vol = NULL`（`dummy_codec.c:135`），板卡管理器也没装软件音量
   handler，`esp_codec_dev_set_out_vol()` 直接返回 `NOT_SUPPORT`。
2. **还会把喇叭弄哑**：板卡管理器**每个外设只有一个 `i2s_chan_handle_t`**。Lua 关闭
   codec 设备时 `esp_codec_dev_close()` → `data_if->enable(false)` → `i2s_channel_disable()`
   关掉的正是语音管线借用的那条通道。此后 `i2s_channel_write` 永远返回
   `ESP_ERR_INVALID_STATE`（`i2s_common.c:1353`，"The channel is not enabled"），
   **没有任何代码会重新使能它**，不重启就好不了。

两处都要治：软件音量在 PCM 送进 DMA 前做衰减；写入遇到 `INVALID_STATE` 时先用
`i2s_channel_get_info()` 确认是"通道被禁用"而非"另有传输在进行"（两者错误码相同），
再重新套用板卡的 clk/slot 配置并使能。**重配置而非只 enable**，因为
`esp_codec_dev_open()` 会把采样率改成对方要的值。

**㉗ `sizeof` 作用在指针上**

播放任务的暂存缓冲从栈数组改成堆分配（修栈溢出）时，`chunk > sizeof(stage)` 没同步改。
`stage` 是 `uint8_t *`，`sizeof` 是 **4** 而不是 8192，于是每轮循环只搬 4 字节、
加解锁一次、写 I2S 一次，24 kHz 下约 12000 次/秒。功能上还能出声，但极可能是断续的来源。

### 6.8 采集与网络的耦合

**㉘ 采集和发送共用一个任务 = 句子中间掉字**

现象：说"声音调高到60%"，识别回来是"声，调高到60%"——**"音"没了**。

老代码是 `读麦克风 → asr_send_frame → TLS 写 → 回来再读`。I2S RX DMA 只存
6 描述符 × 240 帧 = **90 ms**，任何一次 TLS 写超过 90 ms，那段音频就被静默覆盖。
从日志按"按住时长 vs 实际送出音频"能量化：

| 说的话 | 按住 | 送出 | 丢失 |
| --- | --- | --- | --- |
| 那声音调低 | 3421 ms | 2000 ms | **42%** |
| 声音…… | 2833 ms | 1200 ms | **58%** |

最刺眼的一段：握手完成（82660）到预缓冲发完（84230）之间 **1570 ms 麦克风完全没人读**。

正解：**独立高优先级任务持续把麦克风灌进 4 秒 PSRAM 环**，ASR 只管从环里取。
握手预缓冲随之作废——它本来只是在一个点上打补丁。

同时修掉了另一个截断：老代码在按键抬起瞬间 `break` 出流式循环，把"已采集但还没发出去"
的音频直接丢掉，**每句话的最后一个词都被切掉**。现在抬起只停麦克风，继续排空积压。

### 6.9 蓝牙

**㉙ 扫描被拒：`rc=519`，两个不同的原因**

`519 = 0x207 = BLE_HS_ERR_HCI_BASE(0x200) + 0x07`，HCI **"Memory Capacity Exceeded"**。
踩了两次，原因不同：

1. 传了 `filter_duplicates = 1`，但 `CONFIG_BT_CTRL_BLE_SCAN_DUPL` 没开，控制器没有
   去重表 → 拒绝整个扫描。改为软件侧按地址去重。
2. 改完还是 519。真因是 **`CONFIG_BT_CTRL_BLE_MAX_ACT=2`**——Kconfig 原文："BLE maximum
   activities of bluetooth controller，both of connections, scan, sync and adv"，
   默认 6，本工程压到了 2。广播占 1 个槽、连接占 1 个，扫描要第 3 个，超出。
   调到 **4**（广播 + 扫描 + 两条连接），每槽 828 字节。

**教训**：同一个错误码可以有完全不同的成因，第一个假设被证伪后要重新查，而不是打补丁。

**㉚ 未处理 `PASSKEY_ACTION` 会让配对静默挂死**

从手机系统蓝牙设置发起配对时，NimBLE 抛 `BLE_GAP_EVENT_PASSKEY_ACTION` 后**等待应用调
`ble_sm_inject_io()`**。事件落到 `default: return 0` 就没人应答，配对一直挂着，直到手机
放弃并断开，报 `reason=531`（`0x213` = HCI `0x13` "Remote User Terminated"）。

顺带也补了 `REPEAT_PAIRING`：对端带着旧绑定重连时若不删除陈旧密钥并返回 `RETRY`，
第二次配对必然失败。

**注意**：触发特征本来就不需要加密，**用 BLE 调试助手直接连接、不配对是正常路径**。

**㉛ 单击被边沿检测吞掉**

`tap` 走 `ble_trigger_report(source, !listening)`，而后者对"是否按住"做边沿去重
（为了挡 HID 按键重复）。录音到上限**自己结束时没人清那个标志**，于是下一次单击算出的值
与标志相同 → 判为重复 → 丢弃，要等 15 秒超时清掉才恢复。日志里表现为三条
`No release after 15000 ms` 和每次单击后一段死区。

正解：单击不走边沿状态机，自己一条消息，由工作任务直接问
`cap_voice_pipeline_is_listening()`——那才是唯一知道录音有没有在跑的东西。

### 6.10 诊断代码本身也会骗人

**㉜ `button.get_key_level()` 返回的不是引脚电平**

为排查外接按钮加了"静息电平"检查，结果两个按钮都报 `reads 0 at rest, expected 1`，
可其中一个明明工作正常。看源码 `button_gpio.c:42`：

```c
return level == gpio_btn->active_level ? 1 : 0;
```

它**已经按 `active_level` 归一化过了**——返回的是"按下与否"，松开恒为 0。
更糟的是：接地坏的时候上拉把引脚拉到 1、`1 == 0` 为假 → 返回 0；接好之后同样返回 0，
**坏的和好的打印一模一样**，这个检查从头到尾没提供过任何信息。

改成反过来判断：只有开机就读作"已按下"才告警（短路到地/开关卡死），那才是真故障。

**㉝ 能力描述超过 256 字节会被硬截断**

`claw_cap.c:313` 对超长描述按字节硬切。`voice_set_volume` 写了 276 字节，正好把
"别用 Lua audio 模块调音量"那句警告切掉了——**而那恰恰是这段描述存在的意义**。
`display_text` 的 346 字节同理，被砍掉的是"中文和 emoji 显示为空白"。

排查时这条截断警告还立了功：日志里只有 `display_text` 的警告、**没有 `voice_set_volume`
的**，据此断定固件里根本没有那个工具（用户没烧录），而不是它坏了。

**㉞ JSON 数字进 Lua 会变成浮点**

router 规则传 `{"gpios": [0, 21]}`，`cap_lua_push_json_value` 用 `lua_pushnumber`，
日志打成 `hold GPIO 0.0 or 21.0`。`luaL_checkinteger` 对整数值浮点能正常转换，
所以**功能没坏，只是每条关于这个引脚的日志看起来都像另一个引脚**。用 `math.tointeger` 归一。

### 6.11 记忆抽取阻塞回答

**㉟ 每轮回答都在等一次与回答无关的 LLM 调用**

实测五个请求，**每一次 `completion` 都恰好在 `auto_extract` 之后 1 毫秒**：

| 请求 | 出识别文本 | 回答完成 | 耗时 | `auto_extract` 完成 |
| --- | --- | --- | --- | --- |
| 1000000 | 133008 | 235490 | **102.5 s** | 235489 |
| 1000001 | 235894 | 239465 | 3.6 s | 239464 |
| 1000002 | 239714 | 251565 | 11.9 s | 251564 |
| 1000003 | 272981 | 296661 | **23.7 s** | 296660 |
| 1000004 | 330442 | 335230 | 4.8 s | 335229 |

根因在 `claw_memory_session.c:383`：

```c
ESP_LOGI(TAG, "stage note provider waiting request=%" PRIu32, request->request_id);
if (!done_sem || xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
```

**`portMAX_DELAY`，无限等待。** 每轮对话跑两次 LLM——一次回答、一次记忆抽取——
而回答必须等抽取跑完。抽取正常 3~12 秒，那就是每句话白等 3~12 秒。

还有个反馈回路让它更糟：等不到回应就再点一次，每次点击新开一条 ASR 的 TLS 连接加 TTS
播放，全挤在同一个 2.4 GHz 射频上（还跑着 BLE），**越点，在等的那个调用越慢**。
第一次赶上这个叠加，等了 102 秒。

**修复不是加个超时那么简单**：工作任务只负责调 LLM 拿 `llm_text`
（`claw_memory_session.c:278-315`），真正的 `claw_memory_auto_extract_apply_result()`
是在**等待方**调用的（第 369 行）。等待方一超时走人，这条记忆就永远存不进去、被 sweep 回收。
正确做法是把 apply 挪进工作任务，让等待变成纯可选的加速路径。

该模块属于 fork 的上游核心（`claw_memory`），**本分支未改动**，先记录在案。

---

## 7. 构建、烧录与调试

### 7.1 加载 ESP-IDF 环境

**每开一个新终端都要做一次**，否则 `idf.py` 不存在或用错版本：

```bash
export IDF_PATH='/Users/leoshan/.espressif/v6.0.2/esp-idf'
. $IDF_PATH/export.sh
cd /Users/leoshan/ai_coding/esp-project/esp-claw/application/edge_agent
```

### 7.2 什么时候需要全量编译

**绝大多数情况都不需要**，`idf.py build` 增量即可。已验证的行为：

| 改了什么 | 怎么做 | 为什么够 |
| --- | --- | --- |
| `.c` / `.h` | `idf.py build` | 常规增量 |
| Lua 脚本 / `SKILL.md` / router 规则 | `idf.py build` | 暂存目标是 `add_custom_target(... ALL)` 且无 `DEPENDS`，**每次构建都 `rm -rf` 重新拷贝并重打包镜像** |
| `sdkconfig` / `sdkconfig.defaults*` | `idf.py build` | 自动 reconfigure |
| `CMakeLists.txt` / `Kconfig` | `idf.py build` | 自动 reconfigure |
| 换开发板 | `idf.py bmgr -c ./boards -b <板名>` + `idf.py set-target esp32s3` | 重新生成板卡代码 |
| 换 ESP-IDF 版本、增删 managed component | 这时才考虑 `fullclean` | 工具链/依赖树变了 |

验证方法（改完 Lua 后直接查产物，不必烧录）：

```bash
strings build/system.bin | grep "push_to_talk: hold GPIO"
```

> ⚠️ **本工程要少碰 `idf.py fullclean`**：它无法从零 configure，`sdkconfig` 丢过一次，
> 是从 `components/gen_bmgr_codes/sdkconfig.bmgr_board.old` 恢复的。遇到诡异构建问题
> 优先重跑 `idf.py build` 或删单个组件的 obj。

> ⚠️ **`sdkconfig` 优先级高于 `sdkconfig.defaults.board`**。板级默认只在生成新
> `sdkconfig` 时起作用，已存在的值不会被覆盖。所以改配置要**两边都改**：
> `sdkconfig`（当前生效）+ `sdkconfig.defaults.board`（防止将来重新生成时飘回去）。

### 7.3 分区与烧录策略

固件分四块，**按改了什么决定烧哪块**：

| 分区 | 偏移 | 内容 | 什么时候要烧 |
| --- | --- | --- | --- |
| `edge_agent.bin` | `0x20000` | C 代码、`sdkconfig` 生效结果 | 改了 `.c`/`.h`/Kconfig |
| `system.bin` | `0xa20000` | **Lua 脚本、SKILL.md、router 规则种子**（只读） | 改了 Lua / skill / `.recovery` |
| `storage.bin` | `0xcaa000` | DATA 分区初始内容 | **几乎不需要，见下** |
| bootloader / 分区表 | `0x0` / `0x9000` | — | 改了分区表或 bootloader 配置 |

```bash
# 只改了 C 代码
idf.py -p /dev/cu.usbmodem11301 app-flash monitor

# 改了 Lua / skill / router 规则（多数情况用这条）
idf.py -p /dev/cu.usbmodem11301 app-flash system-flash monitor

# 全量（第一次烧录、换分区表、或想把设备恢复出厂）
idf.py -p /dev/cu.usbmodem11301 flash monitor

# 只看日志，不烧录
idf.py -p /dev/cu.usbmodem11301 monitor
```

> ⚠️ **全量 `flash` 会重写 `storage.bin`，清空 `/fatfs`**——会话历史、长期记忆、
> 已配置的 router 规则全部丢失，开机从 `/system/.recovery/` 重新播种。启动日志里出现
> `recovered /system/.recovery/... -> /fatfs/...` 就是发生了这件事。
> **想保住设备数据就别烧 `storage-flash`。**
>
> 反过来说：`.recovery` 里的 router 规则改动**只在 DATA 缺失时才播种**，
> 设备上已有旧文件不会被覆盖。要让新规则生效，要么全量烧一次，要么手动删掉设备上那个文件。

### 7.4 确认烧的是不是新固件

烧录后第一件事看这三行，任何一条对不上就是没烧进去：

```
I (873) app_init: Compile time:     Aug 13 2026 23:37:01     ← 与本地构建时间对得上吗
I (879) app: ESP-Claw git version:  baseline-before-voice-port-44-gf61d8f24
--- Warning: Checksum mismatch between flashed and built applications.   ← 出现这行 = 芯片里不是你刚构建的
```

最后那行是 `idf.py monitor` 自己比对 flash 内容与 `build/edge_agent.bin` 的结果，
**是最直接的判据**。本项目已经因为"只跑了 `monitor` 没跑 `flash`"排查过两次假故障。

### 7.5 串口调用 capability

`esp_console` 的分词器会吃掉双引号并按空格切分，**JSON 必须转义**：

```bash
cap call voice_set_volume "{\"percent\":50}"
cap call ble_trigger_scan "{\"seconds\":10}"
cap call ble_trigger_status "{}"
cap call memory_list "{}"
```

写成 `cap call voice_speak {"text":"hi"}` 会被处理成 `{text:hi}`，报 `invalid json`。

---

## 8. 蓝牙配对与连接

ESP32-S3 **只有 BLE，没有经典蓝牙**（soc_caps 里只有 `SOC_BLE_SUPPORTED`），
所以按钮必须是 BLE HID（HOGP），经典蓝牙的遥控器一律不行。

### 8.1 手机作为按钮（已验证）

板子作**外围**，手机 BLE 调试助手连进来。触发特征**不带加密标志，不需要配对**。

1. 开机日志出现：
   ```
   I ble_trig_stack:  NimBLE up as 'esp-claw-mic', own address type 0
   I ble_trig_periph: Advertising as 'esp-claw-mic'; write 0x01/0x00 to characteristic 7a3f1e01-...
   ```
2. 调试助手搜到 `esp-claw-mic`，**直接连接**（不要点"配对/绑定"）
3. 找到服务 / 特征：

   | | UUID |
   | --- | --- |
   | 服务 | `7a3f1e00-5c2b-4a9d-8e11-2f6b0c9d4a71` |
   | 触发特征 | `7a3f1e01-5c2b-4a9d-8e11-2f6b0c9d4a71` |

4. 写入**一个字节**（⚠️ 把输入格式切到 **HEX**，很多 app 默认发文本会被拒）：

   | 写入 | 效果 |
   | --- | --- |
   | `01` | 按下 → 开始录音 |
   | `00` | 松开 → 停止并转写 |
   | `02` | 单击 → 切换（在录就停，没录就开）|

   读该特征返回当前是否正在录音。

成功时串口出现 `I cap_ble_trigger: [gatt] press -> voice_listen: ESP_OK`。

> 从**手机系统蓝牙设置**发起配对会走另一套流程并要求输入配对码——那条路不需要，
> 且配对码是给手机确认的，往串口敲没有意义。

### 8.2 实体 BLE HID 按钮（未经硬件验证）

板子作**中心**，主动连出去。

1. **让按钮进入广播状态**——多数遥控器要长按某键进配对模式，或刚上电才广播。
   这一步没做对后面全白搭。
2. 触发扫描（开机后 6 秒也会自动扫 8 秒，但那时按钮多半还没就绪）：
   ```bash
   cap call ble_trigger_scan "{\"seconds\":10}"
   ```
   也可以直接对着麦克风说"启动蓝牙扫描"，Agent 会自己调这个工具（已实测）。
3. 返回会列出**每个扫到的设备**，能区分"按钮不兼容"和"不在广播"：
   ```json
   {"ok":true,"hid_button_found":true,
    "devices":"seen 5 device(s)\n  ff:ee:dd:cc:bb:aa rssi=-52 hid=yes name=Shutter"}
   ```
4. 连上后串口出现 `Opening HID button ...` → `HID button connected`，
   按一下按钮应看到 `[hid] press -> voice_listen: ESP_OK`。

**不需要输 PIN**：Just Works 配对（`sm_io_cap = NO_INPUT_OUTPUT`，`sm_mitm = 0`），
密钥存 NVS（`BT_NIMBLE_NVS_PERSIST=y`，最多 4 个绑定），重启不用重新配对。

### 8.3 选购与排错

**买之前先验证按钮语义**：配到电脑上，打开文本编辑器长按。字符连续重复说明它分别上报了
按下和抬起，能做"按住说话"；只出一个字符就只有"单击"语义，只能用切换模式。

| 现象 | 原因 |
| --- | --- |
| 扫描报 `rc=519` | 控制器活动槽不足，见踩坑 ㉙ |
| 扫到设备但 `hid=no` | 不是标准 HOGP 设备，走不了这条路 |
| 扫不到按钮 | 按钮没进广播模式，或已被别的设备连着 |
| 连错设备 | 连的是扫到的第一个 `0x1812`，附近键鼠会抢 |
| 重启后不工作 | 不会自动重连，唤醒按钮后重新 `ble_trigger_scan` |

### 8.4 关键 BLE 配置

| 选项 | 值 | 为什么 |
| --- | --- | --- |
| `BT_NIMBLE_ROLE_CENTRAL` / `ROLE_OBSERVER` / `GATT_CLIENT` | **y** | 工程默认关闭，只有外围角色，连不出去 |
| `BT_NIMBLE_HID_SERVICE` | **y** | 否则 esp_hid 的 `nimble_hidh.c` 编译成空文件 |
| `BT_CTRL_BLE_MAX_ACT` | **4** | 广播 + 扫描 + 两条连接，每槽 828 字节 |
| `BT_NIMBLE_MAX_CONNECTIONS` | **2** | 手机与按钮不互相挤掉 |
| `CLAW_CAP_BLE_TRIGGER_HOLD_TIMEOUT_MS` | 15000 | 断连后麦克风不会常开 |

---

## 9. 配置指南

### 9.1 板卡与构建

```bash
export IDF_PATH='/Users/leoshan/.espressif/v6.0.2/esp-idf'
. $IDF_PATH/export.sh
cd application/edge_agent

idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1_voice
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem11301 flash monitor
```

### 9.2 接线

| 器件 | 引脚 |
| --- | --- |
| INMP441 麦克风 | WS=4, SCK=5, DIN=6（I2S0, 16 kHz, 左声道） |
| MAX98357A 功放 | BCLK=15, LRCK=16, DOUT=7（I2S1, 24 kHz） |
| SSD1306 OLED | SDA=41, SCL=42（I2C, 0x3C 或 0x3D 自动探测） |
| 按键（板载） | BOOT 键 GPIO 0，无需接线 |
| 按键（外接） | **GPIO 21 ↔ GND**，两根线，驱动自动开内部上拉，不需要电阻 |

外接按钮选 GPIO 21 的理由：非 strapping 脚，且未被 flash（26-32）、**八线 PSRAM（33-37）**、
USB（19-20）、UART0（43-44）或本板音频/I2C/LED 占用。同样空闲的还有
1、2、8-14、17、18、39、40、47。

⚠️ 四脚轻触开关**同一侧两脚是常通的**，要接对角线的两个脚。

### 9.3 云端配置

网页配置页 →「语音服务 (ASR / TTS)」，**只需填 1 个 API Key**：

| 字段 | 默认值 |
| --- | --- |
| 语音服务 API Key | *（必填）* 火山控制台 → 语音技术 → API Keys |
| ASR Resource ID | `volc.seedasr.sauc.duration` |
| ASR 服务地址 | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream` |
| TTS Resource ID | `seed-tts-2.0` |
| TTS 音色 | `zh_female_vv_uranus_bigtts` |
| TTS 服务地址 | `https://openspeech.bytedance.com/api/v3/tts/unidirectional` |

### 9.4 关键 Kconfig

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CLAW_CAP_VOICE_PIPELINE_ENABLED` | n（本板卡 y） | 关闭后语音代码完全不编译（约省 10 KB） |
| `CLAW_CAP_VOICE_SPEAKER_SAMPLE_RATE` | 24000 | 必须与板卡 `i2s_audio_out` 一致 |
| `CLAW_CAP_VOICE_RECORD_DURATION_SEC` | 8 | 单次采集上限（流式，不决定缓冲大小） |
| `CONFIG_WS_BUFFER_SIZE` | **4096** | 默认 1024 不足以容纳 ASR 网关的响应头 |
| `CONFIG_CODEC_DUMMY_SUPPORT` | **y** | MAX98357A 无控制接口，需 dummy codec |
| `CLAW_CAP_BLE_TRIGGER_ENABLED` | n（本板卡 y） | 蓝牙无线触发，依赖语音管线 |
| `voice_volume`（NVS，非 Kconfig） | 100 | 软件音量 0-100，网页配置页可改，`voice_set_volume` 可改 |

---

## 10. 性能实测

| 指标 | 数值 |
| --- | --- |
| 松手 → 拿到识别文本 | ~600 ms |
| Agent 思考（含联网搜索） | 3~13 s |
| TTS 合成等待（预缓冲） | ~2.3 s |
| 播放欠载 | −32 ms（即零欠载） |
| 握手期间抢救的音频 | ~800 ms |
| 整轮（简单问题） | ~8 s |

优化历程：

| 阶段 | 合成等待 | 播放欠载 |
| --- | --- | --- |
| 边收边播（初版） | — | **20729 ms**（严重爆音） |
| 全部缓冲后播 | 12465 ms | −47 ms |
| **预缓冲 2 秒流式** | **2329 ms** | **−32 ms** |

---

## 11. 已知限制与后续工作

| 项 | 说明 |
| --- | --- |
| **无唤醒词** | 需引入 ESP-SR / WakeNet：模型占 flash、麦克风需常开、CPU 与内存开销显著 |
| **屏幕只支持 ASCII** | 5×7 字库无中文无 emoji，`display_text` 描述里已声明。中文需要点阵字库 + 更大的 flash 占用 |
| **蓝牙麦克风不可行** | ESP32-S3 **只有 BLE，没有经典蓝牙**，而 A2DP/HFP 都依赖 BR/EDR。可行替代是 USB Audio Class（`usb_host_uac`，原板卡已声明过该依赖） |
| **会话历史无限增长** | 实测三轮从 51 字节涨到 51471 字节（工具结果和文件内容都进历史），直接推高每轮 token 成本与延迟。且**每轮要完整发给 LLM 两次**（回答一次、记忆抽取一次） |
| **记忆抽取阻塞回答** | `claw_memory_session.c:383` 用 `portMAX_DELAY` 等待抽取完成才发回复，实测每轮多等 3~12 秒，最坏一次 **102 秒**。详见踩坑 ㉟。属上游核心模块，本分支未改动 |
| **BLE HID 按钮未经硬件验证** | 无实体按钮可测，手机也无法模拟 HID 外设（Android/iOS 保留 `0x1812`）。只保证编译通过与逻辑自洽 |
| **BLE 不自动重连按钮** | 开机扫一次，按钮休眠时不广播多半扫不到，需手动 `ble_trigger_scan` |
| **BLE 连第一个 HID 设备** | 无地址/名称过滤，附近有键鼠可能连错 |
| **录音上限固定** | 按住说话已绕开该限制；若要做免按键，仍需设备侧 VAD |
| **屏幕未纳入板卡管理器** | 目前由本组件直接驱动 I2C，`board_hardware_info` 查不到它。已用 capability 弥补，但 Lua 的 `display`/`lcd` 模块仍无法使用它 |

---

## 附：提交历史

本分支共 46 个提交。按主题归类：

- **初版移植**（`723e6d59` … `dae7caf7`）：v1 接口 + 自建 I2S + 一串构建修复
- **代码审查后重构**（`6aa6cce1`、`018cd4f5`、`ef392592`）：修复 TTS 解码、ASR 不可达、
  回调抢占、网页白屏、引脚静默冲突
- **迁移到 v3 + 板卡化**（`3920b2c0`、`f2baed8b`）：新板型、板卡管理器音频、v3 API
- **联调修复**（`3e9cf889` … `dd53a5d3`）：WS 缓冲、成功码、slot_mask、回复投递
- **体验优化**（`87cf40a2` … `0d67c35c`）：缓冲播放、触发方式、markdown 清理、
  OLED 状态、预缓冲流式、握手期采集、能力可见性
- **文档**（`90967d89`、`8b5bce71`、`719b2672`）：本设计文档、README 提示、功能清单
- **音量与音频健壮性**（`53a9ce8b`、`8f72c1da`）：软件音量、I2S 通道自愈、
  独立采集任务（修句中吞字）
- **蓝牙无线触发**（`a73337ec`、`a413e35d`、`7b8a74cc`、`f61d8f24`）：`cap_ble_trigger`、
  控制器活动槽、配对应答、单击去重
- **按钮与打磨**（`2eb4c45f`、`6bf221af`、`359c3e27`、`88843eca`）：外接按钮、
  8 秒采集上限、电平检查修正、能力描述长度
