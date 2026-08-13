workspace "ESP-Claw Architecture" "C4 Architecture Model for ESP-Claw Embedded AI Agent Framework" {
    !identifiers hierarchical

    model {
        user = person "End User / Operator" "Interacts with ESP-Claw via IM messaging (WeChat/Feishu/TG/QQ), physical buttons, or voice input."
        developer = person "Embedded Developer" "Deploys firmware, updates Lua scripts, configures boards, and monitors system logs via CLI/Web."

        esp_claw = softwareSystem "ESP-Claw System" "Embedded IoT AI Agent Framework running on Espressif ESP32-S3/P4 SoCs." {
            web_ui = container "Embedded Web Config UI" "Delivers single-page web interface for Wi-Fi and LLM key configuration." "React / TypeScript / HTML"
            
            edge_agent = container "Edge Agent Firmware Runtime" "Main C firmware binary executing FreeRTOS tasks for agent reasoning, event routing, and capability dispatch." "ESP-IDF C / FreeRTOS" {
                main_app = component "Application Shell & Boot" "Initializes NVS, Wi-Fi, file systems, runtime config, and spawns agent tasks." "C / FreeRTOS Task"
                event_router = component "Event Router Engine" "Matches incoming IM/Voice/Timer/CLI events against router_rules.json." "claw_event_router"
                agent_core = component "Agent Core Runtime" "Context assembly, LLM API client, and iterative Tool Call state machine." "claw_core"
                memory_mgr = component "Structured Memory System" "Manages short-term session history, long-term user profile memory, and stage notes." "claw_memory"
                cap_registry = component "Capability Registry" "Registers C capabilities, generates tool JSON schemas, and dispatches Tool Calls." "claw_cap"
                skill_mgr = component "Skill Manager" "Scans and activates built-in and user-installed SKILL.md modules." "claw_skill"
                voice_pipeline = component "Voice Pipeline Capability" "Handles I2S audio I/O, VAD, ASR/TTS streaming, and OLED state UI." "cap_voice_pipeline"
                lua_engine = component "Lua Engine Capability" "Executes dynamic Lua scripts to control GPIO/I2C/SPI peripherals." "cap_lua / Lua 5.4"
                im_connectors = component "IM Platform Connectors" "Handles polling and webhooks for WeChat, Feishu, Telegram, and QQ." "cap_im_platform"
                mcp_connector = component "MCP Client & Server" "Implements Model Context Protocol for remote/local tool exchanges." "cap_mcp_client / cap_mcp_server"
                http_server = component "Embedded HTTP Server" "Serves Web UI assets and handles REST configuration APIs." "http_server"

                main_app -> event_router "Initializes and registers event sources for"
                main_app -> agent_core "Spawns agent task and passes config to"
                event_router -> agent_core "Dispatches matched agent tasks to" "Internal Event Queue"
                event_router -> lua_engine "Executes direct Lua script actions via"
                voice_pipeline -> event_router "Publishes ASR recognized text event to"
                agent_core -> memory_mgr "Retrieves and persists context history with"
                agent_core -> skill_mgr "Retrieves active skills from"
                agent_core -> cap_registry "Dispatches Tool Calls through" "C Function Pointers"
                agent_core -> voice_pipeline "Passes final response text to"
                cap_registry -> voice_pipeline "Invokes audio capture/playback via"
                cap_registry -> lua_engine "Invokes dynamic script execution via"
                cap_registry -> im_connectors "Sends outbound messages via"
                cap_registry -> mcp_connector "Exchanges tool calls via"
                http_server -> main_app "Updates runtime configuration in"
            }

            fatfs_data = container "Writable DATA Partition" "FATFS mount storing user memory, session logs, custom skills, and router rules." "SPI Flash / SD Card (FATFS)" {
                tags "Database"
            }

            system_flash = container "System Read-Only Partition" "Flash image containing built-in skills, Lua standard libraries, and recovery seeds." "ESP32 SPI Flash (/system)" {
                tags "Database"
            }

            audio_hw = container "Audio & Display Hardware" "Hardware peripherals: INMP441 Microphone, MAX98357A Speaker, SSD1306 OLED." "Hardware Peripherals"

            web_ui -> edge_agent.http_server "Sends configuration requests to" "HTTP / REST API"
            edge_agent.memory_mgr -> fatfs_data "Reads from and writes session/profile memory to" "POSIX VFS API"
            edge_agent.event_router -> fatfs_data "Reads router_rules.json from" "POSIX VFS API"
            edge_agent.skill_mgr -> fatfs_data "Loads user skills from" "POSIX VFS API"
            edge_agent.skill_mgr -> system_flash "Reads built-in skills from" "POSIX VFS API"
            edge_agent.lua_engine -> system_flash "Loads standard Lua libraries from" "POSIX VFS API"
            edge_agent.voice_pipeline -> audio_hw "Captures microphone audio and plays back speaker sound with" "I2S Protocol"
            edge_agent.voice_pipeline -> audio_hw "Updates UI status on" "I2C / SSD1306"
        }

        llm_cloud = softwareSystem "Cloud LLM Provider" "OpenAI / Claude / Volcengine Doubao / Qwen / DeepSeek API."
        voice_cloud = softwareSystem "Volcano Speech Cloud" "Volcengine Doubao Streaming ASR & TTS WebSocket Service."
        im_cloud = softwareSystem "IM Platform Servers" "Telegram, Feishu, WeChat, QQ Cloud Servers."
        mcp_remote = softwareSystem "External MCP Servers" "Remote Model Context Protocol Servers providing extra tool APIs."

        user -> im_cloud "Sends messages and commands through"
        user -> esp_claw.audio_hw "Speaks to microphone and listens to speaker on"
        user -> esp_claw.web_ui "Configures Wi-Fi and LLM keys via browser on"
        developer -> esp_claw.edge_agent "Deploys firmware and monitors logs via UART CLI on"

        esp_claw.edge_agent.agent_core -> llm_cloud "Sends prompt context and receives tool calls from" "HTTPS / JSON REST"
        esp_claw.edge_agent.voice_pipeline -> voice_cloud "Streams PCM audio for ASR and receives TTS audio stream from" "WebSockets / HTTP"
        esp_claw.edge_agent.im_connectors -> im_cloud "Polls and sends messages to" "HTTPS REST / Webhooks"
        esp_claw.edge_agent.mcp_connector -> mcp_remote "Dispatches remote tool calls to" "JSON-RPC / SSE"

        production = deploymentEnvironment "Production" {
            node1 = deploymentNode "ESP32-S3 Microcontroller" "ESP32-S3-DevKitC-1 with 16MB Flash" "Hardware Board" {
                node2 = deploymentNode "FreeRTOS RTOS Kernels" "ESP-IDF v5.x RTOS" "Real-Time OS" {
                    containerInstance esp_claw.edge_agent
                }
                node3 = deploymentNode "SPI Flash / SD Memory" "Storage Partition" "Flash Memory" {
                    containerInstance esp_claw.fatfs_data
                    containerInstance esp_claw.system_flash
                }
            }
            node4 = deploymentNode "Hardware Peripherals" "External Modules" "Peripherals" {
                containerInstance esp_claw.audio_hw
            }
        }
    }

    views {
        systemLandscape "esp-claw-landscape" "High-Level System Landscape for ESP-Claw IoT Ecosystem." {
            include *
            autolayout lr
        }

        systemContext esp_claw "esp-claw-context" "System Context Diagram for ESP-Claw System." {
            include *
            autolayout lr
        }

        container esp_claw "esp-claw-containers" "Container Diagram for ESP-Claw Edge Agent System." {
            include *
            autolayout lr
        }

        component esp_claw.edge_agent "esp-claw-edge-agent-components" "Component Diagram for Edge Agent Firmware Runtime." {
            include *
            autolayout lr
        }

        dynamic esp_claw.edge_agent "voice-agent-interaction-flow" "Sequence for Voice Input to Agent Decision and Speech Synthesis Flow." {
            esp_claw.edge_agent.voice_pipeline -> esp_claw.audio_hw "Captures microphone audio and plays back speaker sound with"
            esp_claw.edge_agent.voice_pipeline -> voice_cloud "Streams PCM audio for ASR and receives TTS audio stream from"
            esp_claw.edge_agent.voice_pipeline -> esp_claw.edge_agent.event_router "Publishes ASR recognized text event to"
            esp_claw.edge_agent.event_router -> esp_claw.edge_agent.agent_core "Dispatches matched agent tasks to"
            esp_claw.edge_agent.agent_core -> llm_cloud "Sends prompt context and receives tool calls from"
            esp_claw.edge_agent.agent_core -> esp_claw.edge_agent.cap_registry "Dispatches Tool Calls through"
            esp_claw.edge_agent.agent_core -> esp_claw.edge_agent.voice_pipeline "Passes final response text to"
            esp_claw.edge_agent.voice_pipeline -> voice_cloud "Streams PCM audio for ASR and receives TTS audio stream from"
            esp_claw.edge_agent.voice_pipeline -> esp_claw.audio_hw "Captures microphone audio and plays back speaker sound with"
            autolayout lr
        }

        deployment esp_claw "Production" "esp32s3-devkit-deployment" "Deployment Diagram for ESP32-S3 DevKit Board." {
            include *
            autolayout lr
        }

        styles {
            element "Person" {
                shape person
                background #08427b
                color #ffffff
            }

            element "Software System" {
                background #1168bd
                color #ffffff
            }

            element "Container" {
                background #438dd5
                color #ffffff
            }

            element "Component" {
                background #85bbf0
                color #000000
            }

            element "Database" {
                shape cylinder
                background #2A7B88
                color #ffffff
            }

            relationship "Relationship" {
                color #707070
                thickness 2
            }
        }
    }
}
