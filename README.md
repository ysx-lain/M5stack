# 🤖 M5Stack CoreS3 语音对话机器人
> 基于本地LLM模块的离线语音交互系统，支持网页端控制和多设备管理

---

## ✨ 功能特性
### 🎤 语音交互
- 离线语音识别（ASR）
- 本地LLM对话，无需外网
- 语音合成（TTS）播报
- 对话历史管理

### 🌐 网页端服务
- 局域网Web访问界面
- 响应式设计，支持手机/电脑
- 实时WebSocket通信
- 远程对话控制

### 🎮 传感器交互
- IMU姿态检测
- 摇晃切换表情
- 动态表情系统（4种表情）
- 触摸交互

### 🔌 多设备控制
- 串口控制其他ESP32S3设备
- 设备在线状态管理
- 自定义AT指令发送
- MiniClaw兼容模式

### 📱 多APP界面
- 💬 对话APP：语音/文字对话
- 🎮 控制APP：设备管理与控制
- 😊 表情APP：交互表情展示
- ⚙️ 设置APP：系统参数配置

---

## 🛠️ 硬件要求
| 设备 | 说明 |
|------|------|
| M5Stack CoreS3 | 主控制器 |
| LLM语音模块 | ASR/LLM/TTS一体化模块 |
| (可选) ESP32S3开发板 | 受控设备 |
| WiFi网络 | 局域网连接 |

### 接线说明
| 模块 | CoreS3引脚 |
|------|-----------|
| LLM模块 TX | GPIO 17 |
| LLM模块 RX | GPIO 18 |
| ESP32S3 TX | GPIO 1 |
| ESP32S3 RX | GPIO 2 |
| 电源 | 5V/2A USB-C |

---

## 🚀 快速开始
### 1. 环境配置
1. 安装Arduino IDE 2.0+
2. 添加M5Stack板级支持包：
   ```
   https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
   ```
3. 安装依赖库：
   - M5CoreS3 (≥2.0.0)
   - WebServer
   - WebSockets
   - ArduinoJson

### 2. 固件编译
1. 克隆本仓库：
   ```bash
   git clone https://github.com/your-username/M5-LLM-Robot.git
   ```
2. 打开`src/main.cpp`
3. 修改WiFi配置：
   ```cpp
   const char* ssid = "你的WiFi名称";
   const char* password = "你的WiFi密码";
   ```
4. 选择开发板：`M5Stack CoreS3`
5. 编译并上传到CoreS3

### 3. 使用说明
1. 开机后CoreS3会自动连接WiFi
2. 屏幕显示IP地址，例如：`192.168.1.100`
3. 手机/电脑连接同一WiFi，访问该IP地址即可使用网页端
4. 主界面左右滑动切换APP（预留功能）
5. 对话界面按住M5按钮说话，松开后自动识别并回答
6. 表情界面摇晃设备切换表情

---

## 📡 API接口
### HTTP接口
| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 网页主界面 |
| `/api/chat?text=xxx` | GET | 发送对话请求 |

### WebSocket接口
- 连接地址：`ws://<设备IP>:81`
- 发送消息格式：
  ```json
  {
    "type": "chat",
    "payload": "你好"
  }
  ```
- 接收消息格式：
  ```json
  {
    "type": "chat",
    "payload": {
      "role": "assistant",
      "content": "你好！我是你的语音助手。"
    }
  }
  ```

### LLM模块AT指令
| 指令 | 说明 |
|------|------|
| `AT+INIT` | 初始化模块 |
| `AT+ASR=START` | 启动语音识别 |
| `AT+CHAT=<prompt>` | 发送对话请求 |
| `AT+TTS=<text>` | 启动语音合成 |

---

## 🎨 功能截图
### 主界面
```
┌─────────────────────────────────┐
│ LLM Robot                       │
│ Swipe left/right to switch apps │
│                                 │
│ ┌────────────┐  ┌────────────┐  │
│ │    💬      │  │    🎮      │  │
│ │   Chat     │  │  Control   │  │
│ └────────────┘  └────────────┘  │
│ ┌────────────┐  ┌────────────┐  │
│ │    😊      │  │    ⚙️      │  │
│ │   Emoji    │  │ Settings   │  │
│ └────────────┘  └────────────┘  │
└─────────────────────────────────┘
```

### 对话界面
```
┌─────────────────────────────────┐
│ Voice Chat                    < │
├─────────────────────────────────┤
│ You: 今天天气怎么样？            │
│ Bot: 抱歉，我没有联网功能，无法   │
│      查询天气哦～                │
│                                 │
│                                 │
├─────────────────────────────────┤
│ Hold M5 button to speak        ● │
└─────────────────────────────────┘
```

### 表情界面
```
┌─────────────────────────────────┐
│ Emoji                         < │
│                                 │
│          ╭─────────╮            │
│          │  ●   ●  │            │
│          │         │            │
│          │  ╰───╯  │            │
│          ╰─────────╯            │
│                                 │
│    Shake to change emoji!        │
└─────────────────────────────────┘
```

---

## 🔧 自定义开发
### 添加新APP
1. 在`AppState`枚举中添加新的APP状态
2. 实现对应的`drawXXX()`绘制函数
3. 在`handleTouch()`中添加触摸处理逻辑
4. 在`drawUI()`中添加调用

### 扩展设备控制
1. 在`devices`数组中添加设备信息
2. 实现对应的控制指令协议
3. 在控制界面添加控制按钮

### LLM模块适配
默认使用的是支持AT指令的一体化LLM语音模块，如果使用其他模块，需要修改：
- `initLLMModule()`：初始化逻辑
- `processVoiceInput()`：ASR处理逻辑
- `processLLMResponse()`：对话处理逻辑
- `sendTTS()`：TTS处理逻辑

---

## 📊 性能参数
| 参数 | 值 |
|------|----|
| 语音识别响应时间 | < 2s |
| LLM首字响应时间 | < 3s |
| 网页端加载时间 | < 1s |
| 表情切换延迟 | < 100ms |
| 固件大小 | ~1.2MB |
| 内存占用 | < 150KB |
| 待机电流 | ~120mA |

---

## 🚨 常见问题
### Q: LLM模块连接失败？
A: 检查接线是否正确，确认模块波特率为115200，检查模块供电是否充足。

### Q: WiFi连接失败？
A: 确认WiFi名称和密码正确，仅支持2.4G WiFi，检查WiFi信号强度。

### Q: 语音识别准确率低？
A: 说话时距离麦克风不要太远，避免环境噪音，可调整端点检测参数。

### Q: 网页端无法访问？
A: 确认手机/电脑和设备在同一局域网，检查防火墙设置，确认IP地址正确。

---

## 📄 许可证
MIT License

## 🤝 贡献
欢迎提交Issue和Pull Request！

## 📧 联系方式
如有问题或建议，欢迎提交Issue。

---

**GitHub仓库地址**：[https://github.com/your-username/M5-LLM-Robot](https://github.com/your-username/M5-LLM-Robot)