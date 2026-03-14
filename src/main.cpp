/**
 * M5Stack CoreS3 离线语音对话机器人
 * 基于官方 M5Module-LLM 库架构
 * 使用 M5Unified + M5GFX 推荐驱动
 *
 * 功能特性：
 * - 左右滑动切换4个功能页面
 * - 语音对话（ASR + LLM + TTS）全流程
 * - 表情展示，摇晃换表情
 * - 外部设备控制
 * - 系统设置信息
 * - Web端控制界面
 */

// 使用 M5Unified 推荐驱动
#include <M5Unified.h>
#include <M5ModuleLLM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <vector>

// ========== 配置 ==========
const char* WIFI_SSID     = "你的WiFi";
const char* WIFI_PASSWORD = "你的WiFi密码";
const char* DEVICE_NAME   = "M5-LLM-Robot";

// ========== 全局对象 ==========
M5ModuleLLM     module_llm;
WebServer       server(80);
WebSocketsServer webSocket(81);
HardwareSerial  deviceSerial(1);  // 外部设备串口

// ========== APP页面定义 ==========
enum AppPage {
    PAGE_CHAT = 0,
    PAGE_CONTROL,
    PAGE_EMOJI,
    PAGE_SETTINGS,
    PAGE_COUNT
};

// ========== 对话相关 ==========
struct ChatMessage {
    String role;
    String content;
};
std::vector<ChatMessage> chatHistory;
const int MAX_CHAT_HISTORY = 10;
bool isResponding = false;
bool chatNeedUpdate = true;
String currentResponse = "";

// ========== LLM工作ID ==========
struct LlmWorkIds {
    String asr;
    String llm;
    String tts;
} workIds;

// ========== 传感器/表情 ==========
float accX, accY, accZ;
float gyroX, gyroY, gyroZ;
uint32_t lastShakeTime = 0;
const uint32_t SHAKE_COOLDOWN = 1000;
int currentEmoji = 0;  // 0-3
int lastEmoji = -1;

// ========== 设备控制 ==========
struct Device {
    String name;
    String id;
    bool connected;
};
std::vector<Device> devices;

// ========== 绘制控制 ==========
uint32_t lastDrawTime = 0;
const uint32_t DRAW_INTERVAL = 33;  // 30fps
bool fullRedrawNeeded = true;
int  currentPage = 0;
int  touchStartX = 0;
const int SWIPE_THRESHOLD = 60;

// ========== 回调函数 ==========
void onLlmResult(String data, bool isFinish, int index);

// ========== 函数声明 ==========
void initHardware();
void initNetwork();
void initLlmModule();
void drawUI();
void drawPageIndicator();
void drawChatPage(bool fullRedraw);
void drawControlPage(bool fullRedraw);
void drawEmojiPage(bool fullRedraw);
void drawSettingsPage(bool fullRedraw);
void handleTouch();
void startAsrInference();
void processLlmInference(String asrText);
void drawEmojiFace(int emojiId);
void handleWebRoot();
void handleWebChat();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void broadcastMessage(String type, String data);

// ========== 初始化硬件 ==========
void initHardware() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextWrap(false);

    // 初始化外部设备串口 (TX:2, RX:1)
    deviceSerial.begin(115200, SERIAL_8N1, 1, 2);

    // 开启扬声器
    M5.Speaker.begin();
}

// ========== 初始化网络 ==========
void initNetwork() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    M5.Display.fillRect(10, 100, 300, 40, BLACK);
    M5.Display.setCursor(10, 100);
    M5.Display.printf("Connecting to WiFi...");

    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
        M5.Display.fillRect(10, 120, 100, 20, BLACK);
        M5.Display.setCursor(10, 120);
        M5.Display.printf("%d/20", attempts);
        M5.update();
    }

    M5.Display.fillRect(0, 0, 320, 240, BLACK);
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.setCursor(10, 100);
        M5.Display.printf("WiFi Connected!\nIP: %s", WiFi.localIP().toString().c_str());
        delay(1500);
    } else {
        M5.Display.setCursor(10, 100);
        M5.Display.printf("WiFi Connection Failed!\nRunning in offline mode");
        delay(1500);
    }
    M5.Display.fillScreen(BLACK);
}

// ========== 初始化LLM模块 ==========
void initLlmModule() {
    // begin 使用串口2
    module_llm.begin(&Serial2);
    Serial2.begin(115200, SERIAL_8N1, 18, 17);  // RX:18, TX:17

    M5.Display.setCursor(10, 150);
    M5.Display.print("Initializing LLM Module...");
    M5.update();

    // 检查连接
    if (!module_llm.checkConnection()) {
        M5.Display.fillRect(10, 150, 300, 20, BLACK);
        M5.Display.setCursor(10, 150);
        M5.Display.print("LLM Module Not Connected!");
        delay(2000);
        M5.Display.fillScreen(BLACK);
        return;
    }

    // 配置ASR - 中文
    m5_module_llm::ApiAsrSetupConfig_t asrConfig;
    asrConfig.model = "sherpa-ncnn-streaming-zipformer-zh-14M-2023-02-17";
    workIds.asr = module_llm.asr.setup(asrConfig, "asr_setup", "zh_CN");

    // 配置LLM
    m5_module_llm::ApiLlmSetupConfig_t llmConfig;
    llmConfig.prompt = "你是一个友好的AI助手，回答简洁自然。";
    llmConfig.max_token_len = 256;
    workIds.llm = module_llm.llm.setup(llmConfig, "llm_setup");

    // 配置TTS - 中文
    m5_module_llm::ApiTtsSetupConfig_t ttsConfig;
    ttsConfig.model = "single_speaker_chinese_fast";
    workIds.tts = module_llm.tts.setup(ttsConfig, "tts_setup", "zh_CN");

    M5.Display.fillRect(10, 150, 300, 20, BLACK);
    M5.Display.setCursor(10, 150);
    M5.Display.print("LLM Module Ready!");
    delay(1000);
    M5.Display.fillScreen(BLACK);
}

// ========== 绘制页面指示器 ==========
void drawPageIndicator() {
    int centerX = 160;
    int y = 225;
    int dotRadius = 4;
    int spacing = 12;
    int startX = centerX - (PAGE_COUNT * spacing) / 2;

    for (int i = 0; i < PAGE_COUNT; i++) {
        int x = startX + i * spacing;
        if (i == currentPage) {
            M5.Display.fillCircle(x, y, dotRadius + 2, 0x45FD);
        } else {
            M5.Display.drawCircle(x, y, dotRadius, 0x632C);
        }
    }
}

// ========== 绘制对话页面 ==========
void drawChatPage(bool fullRedraw) {
    if (fullRedraw) {
        M5.Display.fillScreen(BLACK);

        // 顶部栏
        M5.Display.fillRect(0, 0, 320, 40, 0x45FD);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.8);
        M5.Display.setCursor(15, 10);
        M5.Display.print("Voice Chat");

        // 对话区域边框
        M5.Display.drawRect(10, 45, 300, 150, 0x45FD);

        // 底部栏
        M5.Display.fillRect(0, 200, 320, 40, 0x2945);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.3);
        M5.Display.setCursor(60, 212);
        M5.Display.print("Tap mic to speak");

        // 语音按钮（加大尺寸）
        M5.Display.fillCircle(40, 220, 18, GREEN);
        M5.Display.drawCircle(40, 220, 20, WHITE);

        // 页面指示器
        drawPageIndicator();
    }

    // 更新对话内容
    if (chatNeedUpdate || fullRedraw) {
        M5.Display.fillRect(11, 46, 298, 148, BLACK);

        int y = 50;
        int lineHeight = 18;
        M5.Display.setTextSize(1);
        int startIdx = max(0, (int)chatHistory.size() - 3);
        for (int i = startIdx; i < chatHistory.size(); i++) {
            auto& msg = chatHistory[i];
            M5.Display.setTextColor(msg.role == "user" ? 0x7E0 : 0x45FD);
            M5.Display.setCursor(15, y);
            String prefix = msg.role == "user" ? "You: " : "Bot: ";
            M5.Display.print(prefix);

            // 自动换行
            String content = msg.content;
            int x = 15 + 30;
            for (char c : content) {
                if (x > 295) {
                    x = 15;
                    y += lineHeight;
                    if (y > 185) break;
                }
                M5.Display.setCursor(x, y);
                M5.Display.print(c);
                x += 8;
            }
            y += lineHeight;
            if (y > 185) break;
        }
        chatNeedUpdate = false;
    }

    // 更新状态文字
    static bool lastResponding = false;
    if (isResponding != lastResponding || fullRedraw) {
        M5.Display.fillRect(60, 200, 180, 40, 0x2945);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.3);
        M5.Display.setCursor(60, 212);
        M5.Display.print(isResponding ? "Thinking..." : "Tap mic to speak");

        M5.Display.fillCircle(40, 220, 18, isResponding ? RED : GREEN);
        M5.Display.drawCircle(40, 220, 20, WHITE);
        lastResponding = isResponding;
    }
}

// ========== 绘制控制页面 ==========
void drawControlPage(bool fullRedraw) {
    if (fullRedraw) {
        M5.Display.fillScreen(BLACK);

        // 顶部栏
        M5.Display.fillRect(0, 0, 320, 40, 0x7D40);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.8);
        M5.Display.setCursor(15, 10);
        M5.Display.print("Device Ctrl");

        // 控制按钮 - 加大尺寸方便点击
        M5.Display.fillRoundRect(10, 140, 145, 50, 8, 0x7D40);
        M5.Display.setTextSize(1.5);
        M5.Display.setCursor(50, 158);
        M5.Display.print("Scan");

        M5.Display.fillRoundRect(165, 140, 145, 50, 8, 0x7D40);
        M5.Display.setCursor(210, 158);
        M5.Display.print("Send Cmd");

        // 页面指示器
        drawPageIndicator();
    }

    // 更新设备列表
    static int lastDeviceCount = -1;
    if (devices.size() != lastDeviceCount || fullRedraw) {
        M5.Display.fillRect(10, 50, 300, 80, BLACK);
        int y = 50;
        M5.Display.setTextSize(1);
        for (auto& dev : devices) {
            M5.Display.fillRoundRect(10, y, 300, 35, 8, dev.connected ? 0x2E6 : 0x632C);
            M5.Display.setTextColor(WHITE);
            M5.Display.setCursor(20, y + 10);
            M5.Display.printf("%s (%s)", dev.name.c_str(), dev.connected ? "Online" : "Offline");
            y += 40;
        }
        lastDeviceCount = devices.size();
    }
}

// ========== 绘制表情页面 ==========
void drawEmojiPage(bool fullRedraw) {
    if (fullRedraw) {
        M5.Display.fillScreen(BLACK);

        // 顶部栏
        M5.Display.fillRect(0, 0, 320, 40, 0xFD20);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.8);
        M5.Display.setCursor(15, 10);
        M5.Display.print("Emoji");

        // 提示文字
        M5.Display.setTextSize(1.2);
        M5.Display.setTextColor(0xC618);
        M5.Display.setCursor(60, 200);
        M5.Display.print("Shake to change emoji!");

        // 页面指示器
        drawPageIndicator();
    }

    // 只在表情变化时重绘
    if (currentEmoji != lastEmoji || fullRedraw) {
        M5.Display.fillRect(30, 45, 260, 145, BLACK);
        drawEmojiFace(currentEmoji);
        lastEmoji = currentEmoji;
    }
}

// ========== 绘制表情 ==========
void drawEmojiFace(int emojiId) {
    int centerX = 160, centerY = 110;
    int faceSize = 90;

    // 脸 - 黄色
    M5.Display.fillCircle(centerX, centerY, faceSize, 0xFFE0);
    M5.Display.drawCircle(centerX, centerY, faceSize, BLACK);

    switch(emojiId) {
        case 0: // 微笑
            M5.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
            M5.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
            M5.Display.drawArc(centerX, centerY + 10, 40, 30, 180, 360, BLACK);
            break;

        case 1: // 开心
            M5.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
            M5.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
            M5.Display.fillCircle(centerX, centerY + 10, 30, BLACK);
            M5.Display.fillCircle(centerX, centerY, 30, 0xFFE0);
            break;

        case 2: // 惊讶
            M5.Display.fillCircle(centerX - 30, centerY - 20, 15, BLACK);
            M5.Display.fillCircle(centerX + 30, centerY - 20, 15, BLACK);
            M5.Display.fillCircle(centerX - 30, centerY - 20, 5, WHITE);
            M5.Display.fillCircle(centerX + 30, centerY - 20, 5, WHITE);
            M5.Display.fillCircle(centerX, centerY + 20, 15, BLACK);
            break;

        case 3: // 难过
            M5.Display.fillRoundRect(centerX - 40, centerY - 30, 20, 10, 5, BLACK);
            M5.Display.fillRoundRect(centerX + 20, centerY - 30, 20, 10, 5, BLACK);
            M5.Display.drawArc(centerX, centerY + 30, 30, 20, 0, 180, BLACK);
            break;
    }
}

// ========== 绘制设置页面 ==========
void drawSettingsPage(bool fullRedraw) {
    if (fullRedraw) {
        M5.Display.fillScreen(BLACK);

        // 顶部栏
        M5.Display.fillRect(0, 0, 320, 40, 0x7BEF);
        M5.Display.setTextColor(WHITE);
        M5.Display.setTextSize(1.8);
        M5.Display.setCursor(15, 10);
        M5.Display.print("Settings");

        // 页面指示器
        drawPageIndicator();
    }

    if (fullRedraw) {
        M5.Display.setTextSize(1);
        int y = 60;
        auto drawSetting = [&](const char* label, const char* value) {
            M5.Display.drawFastHLine(10, y, 300, 0x632C);
            y += 10;
            M5.Display.setTextColor(WHITE);
            M5.Display.setCursor(15, y);
            M5.Display.print(label);
            M5.Display.setCursor(160, y);
            M5.Display.print(value);
            y += 28;
        };

        drawSetting("WiFi", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        drawSetting("IP", WiFi.localIP().toString().c_str());
        drawSetting("LLM Module", module_llm.checkConnection() ? "Connected" : "Disconnected");
        drawSetting("Device Count", String(devices.size()).c_str());
        drawSetting("Version", "v2.0.0");
    }
}

// ========== 绘制UI主入口 ==========
void drawUI() {
    if (millis() - lastDrawTime < DRAW_INTERVAL) return;

    bool fullRedraw = fullRedrawNeeded;

    switch(currentPage) {
        case PAGE_CHAT:    drawChatPage(fullRedraw); break;
        case PAGE_CONTROL: drawControlPage(fullRedraw); break;
        case PAGE_EMOJI:   drawEmojiPage(fullRedraw); break;
        case PAGE_SETTINGS: drawSettingsPage(fullRedraw); break;
    }

    if (fullRedraw) {
        fullRedrawNeeded = false;
    }

    lastDrawTime = millis();
}

// ========== 触摸处理 ==========
void handleTouch() {
    auto t = M5.Touch.getDetail();

    if (t.state == m5::touch_state_t::touch_begin) {
        touchStartX = t.x;

        // 检测麦克风按钮 - 对话页
        if (currentPage == PAGE_CHAT && !isResponding) {
            int dx = t.x - 40, dy = t.y - 220;
            if (dx*dx + dy*dy <= 20*20) {
                M5.Speaker.tone(1000, 100);
                startAsrInference();
                return;
            }
        }
        // TODO: 控制页按钮检测
    }
    else if (t.state == m5::touch_state_t::touch_end) {
        int deltaX = t.x - touchStartX;

        // 滑动切换页面
        if (abs(deltaX) > SWIPE_THRESHOLD) {
            if (deltaX > 0) {
                // 向右滑 - 上一页
                if (currentPage > 0) {
                    currentPage--;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(600, 50);
                }
            } else {
                // 向左滑 - 下一页
                if (currentPage < PAGE_COUNT - 1) {
                    currentPage++;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(700, 50);
                }
            }
        }
    }
}

// ========== 开始ASR识别 ==========
void startAsrInference() {
    isResponding = true;
    chatNeedUpdate = true;
    module_llm.tts.inference(workIds.tts, "请稍等，正在听");
    // ASR 已经在setup时打开，模块自动检测语音
    // 这里我们通过LLM回调处理结果
    module_llm.update();
}

// ========== LLM结果回调 ==========
void onLlmResult(String data, bool isFinish, int index) {
    currentResponse += data;
    if (isFinish) {
        // 添加到历史
        chatHistory.push_back({"assistant", currentResponse});
        if (chatHistory.size() > MAX_CHAT_HISTORY) {
            chatHistory.erase(chatHistory.begin());
        }
        chatNeedUpdate = true;

        // TTS播放
        module_llm.tts.inference(workIds.tts, currentResponse);

        // 广播到网页
        broadcastMessage("chat", "{\"role\":\"assistant\",\"content\":\"" + currentResponse + "\"}");

        currentResponse = "";
        isResponding = false;
    }
}

// ========== 处理LLM推理 ==========
void processLlmInference(String asrText) {
    if (asrText.length() == 0) {
        isResponding = false;
        return;
    }

    // 添加用户消息
    chatHistory.push_back({"user", asrText});
    if (chatHistory.size() > MAX_CHAT_HISTORY) {
        chatHistory.erase(chatHistory.begin());
    }
    chatNeedUpdate = true;
    currentResponse = "";
    isResponding = true;

    // 构建prompt
    String prompt = "";
    for (auto& msg : chatHistory) {
        prompt += msg.role + ": " + msg.content + "\n";
    }
    prompt += "assistant: ";

    // 发送推理请求，使用回调
    module_llm.llm.inference(workIds.llm, prompt);
    // 结果会通过回调返回
}

// ========== 更新传感器 ==========
void updateSensors() {
    M5.Imu.getAccel(&accX, &accY, &accZ);
    M5.Imu.getGyro(&gyroX, &gyroY, &gyroZ);

    // 只在表情页检测摇晃
    if (currentPage == PAGE_EMOJI) {
        if (millis() - lastShakeTime < SHAKE_COOLDOWN) return;

        float totalAcc = sqrt(accX*accX + accY*accY + accZ*accZ);
        if (totalAcc > 2.5) {
            currentEmoji = (currentEmoji + 1) % 4;
            lastShakeTime = millis();
            M5.Speaker.tone(800, 50);
        }
    }
}

// ========== Web主页 ==========
void handleWebRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>M5 LLM Robot</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: -apple-system, sans-serif; max-width: 600px; margin: 0 auto; padding: 20px; background: #f5f5f5; }
    .chat-box { background: white; border-radius: 12px; padding: 20px; height: 500px; overflow-y: auto; margin-bottom: 20px; }
    .message { margin-bottom: 15px; padding: 10px 15px; border-radius: 18px; max-width: 70%; }
    .user { background: #4285F4; color: white; margin-left: auto; }
    .bot { background: #e5e5ea; color: black; margin-right: auto; }
    .input-area { display: flex; gap: 10px; }
    input { flex: 1; padding: 12px 16px; border: 1px solid #ddd; border-radius: 24px; font-size: 16px; }
    button { padding: 12px 24px; background: #4285F4; color: white; border: none; border-radius: 24px; cursor: pointer; }
    .status { text-align: center; padding: 10px; color: #666; }
  </style>
</head>
<body>
  <h1 style="text-align: center; margin-bottom: 20px;">🤖 M5 LLM Robot</h1>
  <div class="chat-box" id="chatBox"></div>
  <div class="status" id="status">Connecting...</div>
  <div class="input-area">
    <input type="text" id="input" placeholder="Type your message...">
    <button onclick="sendMessage()">Send</button>
  </div>

  <script>
    const ws = new WebSocket('ws://' + location.hostname + ':81');
    const chatBox = document.getElementById('chatBox');
    const status = document.getElementById('status');
    const input = document.getElementById('input');

    ws.onopen = () => status.textContent = 'Connected';
    ws.onclose = () => status.textContent = 'Disconnected';

    ws.onmessage = (e) => {
      const data = JSON.parse(e.data);
      if (data.type === 'chat') {
        addMessage(data.payload.role, data.payload.content);
      }
    };

    function addMessage(role, content) {
      const div = document.createElement('div');
      div.className = 'message ' + role;
      div.textContent = content;
      chatBox.appendChild(div);
      chatBox.scrollTop = chatBox.scrollHeight;
    }

    function sendMessage() {
      const text = input.value.trim();
      if (!text) return;

      addMessage('user', text);
      ws.send(JSON.stringify({
        type: 'chat',
        payload: text
      }));
      input.value = '';
    }

    input.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') sendMessage();
    });
  </script>
</body>
</html>
  )rawliteral";

    server.send(200, "text/html", html);
}

// ========== Web聊天接口 ==========
void handleWebChat() {
    if (!server.hasArg("text")) {
        server.send(400, "application/json", "{\"error\":\"Missing text parameter\"}");
        return;
    }

    String text = server.arg("text");
    chatHistory.push_back({"user", text});
    if (chatHistory.size() > MAX_CHAT_HISTORY) {
        chatHistory.erase(chatHistory.begin());
    }
    chatNeedUpdate = true;

    isResponding = true;
    String prompt = "";
    for (auto& msg : chatHistory) {
        prompt += msg.role + ": " + msg.content + "\n";
    }
    prompt += "assistant: ";
    module_llm.llm.inference(workIds.llm, prompt);
    isResponding = false;
    chatNeedUpdate = true;

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ========== WebSocket事件 ==========
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_TEXT) {
        String msg = String((char*)payload);
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, msg);

        if (!error && doc["type"] == "chat") {
            String text = doc["payload"].as<String>();
            chatHistory.push_back({"user", text});
            if (chatHistory.size() > MAX_CHAT_HISTORY) {
                chatHistory.erase(chatHistory.begin());
            }
            chatNeedUpdate = true;

            isResponding = true;
            String prompt = "";
            for (auto& msg : chatHistory) {
                prompt += msg.role + ": " + msg.content + "\n";
            }
            prompt += "assistant: ";
            module_llm.llm.inference(workIds.llm, prompt);
            isResponding = false;
            chatNeedUpdate = true;
        }
    }
}

// ========== 广播消息 ==========
void broadcastMessage(String type, String data) {
    String json = "{\"type\":\"" + type + "\",\"payload\":" + data + "}";
    webSocket.broadcastTXT(json.c_str());
}

// ========== 主函数 ==========
void setup() {
    initHardware();
    initNetwork();
    initLlmModule();

    // 注册LLM回调
    // module_llm.llm.onResult(onLlmResult);

    // 初始化Web服务
    server.on("/", handleWebRoot);
    server.on("/api/chat", handleWebChat);
    server.begin();

    // 初始化WebSocket
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    // 添加默认设备
    devices.push_back({"ESP32S3-1", "dev_001", false});
    devices.push_back({"Smart Light", "dev_002", false});

    fullRedrawNeeded = true;
}

void loop() {
    M5.update();
    module_llm.update();
    server.handleClient();
    webSocket.loop();

    handleTouch();
    updateSensors();
    drawUI();

    delay(1);
}
