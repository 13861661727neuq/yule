# QQ × DeepSeek 智能自动回复机器人

一个用 **C++** 写的本地插件，把 **DeepSeek** 大模型接入你的 **QQ 账号**，实现自动智能回复。基于 OneBot v11「正向 WebSocket」协议，兼容 **NapCat**、**Lagrange.Core**、**go-cqhttp** 等主流 QQ 机器人框架。

---

## 一、它是什么 / 能做什么

| 能力 | 说明 |
| --- | --- |
| 🤖 智能回复 | 私聊、群聊中自动调用 DeepSeek 生成自然回复 |
| ✅ 明确的开关 | 一条消息即可「开启 / 关闭」自动回复，状态实时显示并**持久化**（重启不丢） |
| 📊 状态查询 | 随时查看当前开关、触发方式、模型、上下文条数 |
| 🧠 多轮上下文 | 每个会话独立记忆最近 N 条对话 |
| 🛡️ 防死循环 | 自动忽略自己发的消息，支持冷却时间 |

---

## 二、整体架构（原理）

```
 你的 QQ 账号
     │
     ▼
NapCat / Lagrange  (OneBot v11 正向 WebSocket 服务端，监听 ws://127.0.0.1:3001)
     │  (WebSocket)
     ▼
qq_deepseek_bot.exe  (本插件：收到消息事件 → 调 DeepSeek → 发回回复)
     │  (HTTPS)
     ▼
DeepSeek API  (https://api.deepseek.com)
```

> 本插件本身**不是** QQ 客户端，它需要一个 OneBot 桥接程序把你已登录的 QQ 暴露成标准协议。推荐用 [NapCat](https://github.com/NapNeko/NapCatQQ)（基于 NTQQ），或 [Lagrange.Core](https://github.com/LagrangeDev/Lagrange.Core)。

---

## 三、使用前准备

1. **Windows 10/11**（本插件使用系统自带 WinHTTP，无需额外安装运行库）。
2. **一个 OneBot v11 桥接**，并开启「正向 WebSocket」：
   - NapCat：在 WebUI 里「网络配置 → 新建 WebSocket 服务端」，地址 `127.0.0.1`，端口 `3001`（或自定）。
   - Lagrange：配置文件里启用 `ForwardWebSocket`，`ws://127.0.0.1:3001`。
3. **DeepSeek API Key**：到 [platform.deepseek.com](https://platform.deepseek.com) 注册并创建密钥（形如 `sk-xxxx`）。

---

## 四、编译

> 需要已安装 **Visual Studio 2022** 的「使用 C++ 的桌面开发」工作负载（含 MSVC）。

双击运行 **`build.bat`**，成功后会在目录下生成 **`qq_deepseek_bot.exe`**。

```bat
build.bat
```

---

## 五、配置

编辑 **`config.json`**（与 exe 放在同一目录，程序优先读取当前目录）：

```jsonc
{
  "deepseek": {
    "api_key": "sk-你的密钥",          // 必填：DeepSeek 密钥
    "base_url": "https://api.deepseek.com",
    "model": "deepseek-chat",           // deepseek-chat 或 deepseek-reasoner
    "system_prompt": "你是一个友好、简洁、准确的 QQ 智能助手，始终用中文回答。",
    "temperature": 0.7,
    "max_tokens": 1024
  },
  "onebot": {
    "ws_url": "ws://127.0.0.1:3001"     // OneBot 正向 WebSocket 地址
  },
  "bot": {
    "auto_reply_enabled": true,         // 启动时自动回复是否开启
    "reply_trigger": "at_me",           // "at_me" 仅@我/关键词触发；"all" 全部消息都回复
    "reply_to_self": false,             // 是否回复自己发的消息（建议 false）
    "history_size": 10,                 // 每会话保留的上下文条数
    "cooldown_seconds": 1,              // 同一会话最小回复间隔（秒）
    "timeout_seconds": 60,              // DeepSeek 请求超时（秒）
    "admin_qq": 0,                      // 0=所有人可开关；否则仅该 QQ 号可开关
    "wake_words": []                    // 群聊关键词（命中即回复），如 ["小助手", "AI"]
  },
  "log": { "level": "info" }            // debug / info / warn / error
}
```

---

## 六、运行与日常使用

1. 先启动 OneBot（NapCat/Lagrange）并登录 QQ。
2. 双击 **`qq_deepseek_bot.exe`**（或在命令行运行），控制台出现彩色横幅即启动成功。

### 🎛️ 自动回复开关（核心）

在 **QQ 里**给机器人发以下命令即可（群聊需先 `@机器人`）：

| 你想做的事 | 发送内容 | 机器人回复（开关标识） |
| --- | --- | --- |
| 开启自动回复 | `开启自动回复`（或 `/on`） | ✅ 自动回复已开启 |
| 关闭自动回复 | `关闭自动回复`（或 `/off`） | ⛔ 自动回复已关闭 |
| 查看状态 | `状态`（或 `/status`） | 状态卡片（含当前开关） |
| 查看帮助 | `帮助`（或 `/help`） | 使用说明 |

> - **开关标识非常明确**：控制台横幅会以 🟢「开启」/ 🔴「关闭」彩色显示；`状态` 命令和每次开关操作也都会返回带 ✅ / ⛔ 的状态文字。
> - 开关状态会**自动写回 config.json**，重启程序后仍然保持。

### 💬 触发规则

- **私聊**：开关打开时，每条消息都回复。
- **群聊**：
  - `reply_trigger = "at_me"`（默认）：只有 **@机器人** 或命中 `wake_words` 关键词时才回复；
  - `reply_trigger = "all"`：群内所有消息都回复（慎用，可能刷屏）。

---

## 七、常见问题

| 现象 | 解决 |
| --- | --- |
| 控制台一直提示「无法连接 OneBot」 | 确认 OneBot 已启动、正向 WS 端口正确，且与 `ws_url` 一致 |
| 回复「自动回复失败：HTTP 状态 401」 | `api_key` 填错或未填，检查 config.json |
| 回复「自动回复失败：…timeout」 | 调大 `timeout_seconds`，或检查网络能否访问 api.deepseek.com |
| 群聊不回复 | 默认要 **@机器人**；或把 `reply_trigger` 改成 `all` |
| 中文乱码 | 用 Windows Terminal / 支持 UTF-8 的终端运行；程序已自动设置 UTF-8 代码页 |

---

## 八、文件清单

```
qq-deepseek-bot/
├── src/qq_deepseek_bot.cpp   # 全部源码（单文件，已详尽注释）
├── third_party/json.hpp      # nlohmann/json（仅此一个第三方头文件，已附带）
├── config.json               # 配置文件
├── build.bat                 # 一键编译脚本（MSVC）
└── README.md                 # 本文档
```

> 安全提示：请勿把含真实密钥的 `config.json` 上传到公开仓库。
