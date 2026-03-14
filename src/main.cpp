/**
 * M5Stack CoreS3 语音对话机器人
 * 功能：语音对话 + 网页交互 + 传感器表情 + ESP32控制
 * 硬件：M5Stack CoreS3 + LLM模块
 */

#include <M5CoreS3.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <vector>

// 配置信息
const char* ssid = "你的WiFi";
const char* password = "你的WiFi密码";
const char* deviceName = "M5-LLM-Robot";

// 全局对象
WebServer server(80);
WebSocketsServer webSocket(81);
HardwareSerial LLMModule(2); // LLM模块使用串口2
HardwareSerial DeviceSerial(1); // 外部设备串口

// 状态枚举
enum AppState {
  APP_HOME,
  APP_CHAT,
  APP_CONTROL,
  APP_EMOJI,
  APP_SETTINGS
};
AppState currentApp = APP_HOME;
AppState lastApp = APP_HOME;

// 对话相关
struct ChatMessage {
  String role;
  String content;
  uint32_t timestamp;
};
std::vector<ChatMessage> chatHistory;
const int MAX_HISTORY = 10;
String currentResponse = "";
bool isResponding = false;

// 传感器相关
float accX, accY, accZ;
float gyroX, gyroY, gyroZ;
uint32_t lastShakeTime = 0;
const uint32_t SHAKE_COOLDOWN = 1000;
int currentEmoji = 0; // 0: 微笑, 1: 开心, 2: 惊讶, 3: 难过

// 设备控制相关
struct Device {
  String name;
  String id;
  bool connected;
};
std::vector<Device> devices;

// UI相关
bool needRedraw = true;
uint32_t lastDrawTime = 0;
const uint32_t DRAW_INTERVAL = 30;

// 触摸相关
int touchX = -1, touchY = -1;
bool touchPressed = false;
int touchStartX = -1;
const int SWIPE_THRESHOLD = 50;

// 函数声明
void initHardware();
void initNetwork();
void initLLMModule();
void drawUI();
void drawHome();
void drawChat();
void drawControl();
void drawEmoji();
void drawSettings();
void handleTouch();
void handleSwipe(int deltaX);
void processVoiceInput();
void processLLMResponse();
void sendTTS(String text);
void updateSensors();
void checkShake();
void drawEmojiFace(int emojiId);
void handleWebRoot();
void handleWebChat();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void broadcastMessage(String type, String data);

// 初始化硬件
void initHardware() {
  auto cfg = M5.config();
  CoreS3.begin(cfg);
  CoreS3.Display.setRotation(1);
  CoreS3.Display.fillScreen(BLACK);
  CoreS3.Display.setTextColor(WHITE);
  
  // 初始化LLM串口 (TX:17, RX:18)
  LLMModule.begin(115200, SERIAL_8N1, 18, 17);
  
  // 初始化设备控制串口 (TX:2, RX:1)
  DeviceSerial.begin(115200, SERIAL_8N1, 1, 2);
  
  // 初始化IMU
  CoreS3.Imu.begin();
  
  // 初始化麦克风
  CoreS3.Microphone.begin();
  
  // 初始化扬声器
  CoreS3.Speaker.begin();
}

// 初始化网络
void initNetwork() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    CoreS3.Display.setCursor(10, 100);
    CoreS3.Display.printf("Connecting to WiFi... %d/20", attempts);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    CoreS3.Display.fillScreen(BLACK);
    CoreS3.Display.setCursor(10, 100);
    CoreS3.Display.printf("WiFi Connected!\nIP: %s", WiFi.localIP().toString().c_str());
    delay(2000);
  } else {
    CoreS3.Display.fillScreen(BLACK);
    CoreS3.Display.setCursor(10, 100);
    CoreS3.Display.printf("WiFi Connection Failed!\nRunning in offline mode");
    delay(2000);
  }
}

// 初始化LLM模块
void initLLMModule() {
  // 发送初始化命令
  LLMModule.println("AT+INIT");
  delay(1000);
  while (LLMModule.available()) {
    String resp = LLMModule.readStringUntil('\n');
    if (resp.indexOf("OK") != -1) {
      CoreS3.Display.setCursor(10, 150);
      CoreS3.Display.print("LLM Module Connected!");
      delay(1000);
      return;
    }
  }
  CoreS3.Display.setCursor(10, 150);
  CoreS3.Display.print("LLM Module Not Found!");
  delay(2000);
}

// 绘制主界面
void drawHome() {
  // 背景渐变
  for (int i = 0; i < 240; i++) {
    CoreS3.Display.drawFastHLine(0, i, 320, CoreS3.Display.color565(20 + i/8, 20 + i/8, 30 + i/6));
  }
  
  // 标题
  CoreS3.Display.setTextSize(3);
  CoreS3.Display.setCursor(20, 20);
  CoreS3.Display.print("LLM Robot");
  
  // 副标题
  CoreS3.Display.setTextSize(1.2);
  CoreS3.Display.setTextColor(0xC618);
  CoreS3.Display.setCursor(20, 50);
  CoreS3.Display.print("Swipe left/right to switch apps");
  
  // APP卡片
  int cardW = 130, cardH = 130;
  int spacing = 20;
  int startX = (320 - (cardW * 2 + spacing)) / 2;
  int cardY = 70;
  
  // 第一行
  auto drawCard = [&](int x, int y, uint16_t color, const char* icon, const char* label) {
    CoreS3.Display.fillRoundRect(x + 2, y + 2, cardW, cardH, 16, 0x18C3);
    CoreS3.Display.fillRoundRect(x, y, cardW, cardH, 16, color);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(4);
    CoreS3.Display.setCursor(x + (cardW - 48) / 2, y + 25);
    CoreS3.Display.print(icon);
    CoreS3.Display.setTextSize(1.2);
    CoreS3.Display.setCursor(x + (cardW - 40) / 2, y + 100);
    CoreS3.Display.print(label);
  };
  
  drawCard(startX, cardY, 0x45FD, "💬", "Chat");
  drawCard(startX + cardW + spacing, cardY, 0x7D40, "🎮", "Control");
  
  // 第二行
  drawCard(startX, cardY + cardH + spacing, 0xFD20, "😊", "Emoji");
  drawCard(startX + cardW + spacing, cardY + cardH + spacing, 0x7BEF, "⚙️", "Settings");
  
  // 页面指示器
  CoreS3.Display.fillCircle(150, 225, 3, WHITE);
  CoreS3.Display.drawCircle(170, 225, 3, 0x632C);
}

// 绘制对话界面
void drawChat() {
  CoreS3.Display.fillScreen(BLACK);
  
  // 顶部栏
  CoreS3.Display.fillRect(0, 0, 320, 30, 0x45FD);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(1.5);
  CoreS3.Display.setCursor(10, 7);
  CoreS3.Display.print("Voice Chat");
  
  // 返回按钮
  CoreS3.Display.fillCircle(300, 15, 10, RED);
  CoreS3.Display.setCursor(296, 8);
  CoreS3.Display.print("<");
  
  // 对话区域
  int chatY = 35;
  int chatH = 170;
  CoreS3.Display.drawRect(10, chatY, 300, chatH, 0x45FD);
  
  // 显示最近对话
  int y = chatY + 5;
  int lineHeight = 18;
  CoreS3.Display.setTextSize(1);
  for (int i = max(0, (int)chatHistory.size() - 3); i < chatHistory.size(); i++) {
    auto& msg = chatHistory[i];
    CoreS3.Display.setTextColor(msg.role == "user" ? 0x7E0 : 0x45FD);
    CoreS3.Display.setCursor(15, y);
    String prefix = msg.role == "user" ? "You: " : "Bot: ";
    CoreS3.Display.print(prefix);
    
    // 自动换行
    String content = msg.content;
    int x = 15 + 30;
    for (char c : content) {
      if (x > 295) {
        x = 15;
        y += lineHeight;
        if (y > chatY + chatH - lineHeight) break;
      }
      CoreS3.Display.setCursor(x, y);
      CoreS3.Display.print(c);
      x += 8;
    }
    y += lineHeight;
    if (y > chatY + chatH - lineHeight) break;
  }
  
  // 底部输入提示
  CoreS3.Display.fillRect(0, 210, 320, 30, 0x2945);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setCursor(10, 218);
  CoreS3.Display.print(isResponding ? "Thinking..." : "Hold M5 button to speak");
  
  // 语音按钮
  CoreS3.Display.fillCircle(280, 225, 12, isResponding ? RED : GREEN);
}

// 绘制控制界面
void drawControl() {
  CoreS3.Display.fillScreen(BLACK);
  
  // 顶部栏
  CoreS3.Display.fillRect(0, 0, 320, 30, 0x7D40);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(1.5);
  CoreS3.Display.setCursor(10, 7);
  CoreS3.Display.print("Device Control");
  
  // 返回按钮
  CoreS3.Display.fillCircle(300, 15, 10, RED);
  CoreS3.Display.setCursor(296, 8);
  CoreS3.Display.print("<");
  
  // 设备列表
  CoreS3.Display.setTextSize(1);
  int y = 40;
  for (auto& dev : devices) {
    CoreS3.Display.fillRoundRect(10, y, 300, 30, 8, dev.connected ? 0x2E6 : 0x632C);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setCursor(20, y + 8);
    CoreS3.Display.printf("%s (%s)", dev.name.c_str(), dev.connected ? "Online" : "Offline");
    y += 40;
  }
  
  // 控制按钮
  CoreS3.Display.fillRoundRect(10, 180, 145, 40, 8, 0x7D40);
  CoreS3.Display.setCursor(50, 192);
  CoreS3.Display.print("Scan");
  
  CoreS3.Display.fillRoundRect(165, 180, 145, 40, 8, 0x7D40);
  CoreS3.Display.setCursor(210, 192);
  CoreS3.Display.print("Send Cmd");
}

// 绘制表情界面
void drawEmoji() {
  CoreS3.Display.fillScreen(BLACK);
  
  // 顶部栏
  CoreS3.Display.fillRect(0, 0, 320, 30, 0xFD20);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(1.5);
  CoreS3.Display.setCursor(10, 7);
  CoreS3.Display.print("Emoji");
  
  // 返回按钮
  CoreS3.Display.fillCircle(300, 15, 10, RED);
  CoreS3.Display.setCursor(296, 8);
  CoreS3.Display.print("<");
  
  // 绘制表情
  drawEmojiFace(currentEmoji);
  
  // 提示
  CoreS3.Display.setTextSize(1.2);
  CoreS3.Display.setTextColor(0xC618);
  CoreS3.Display.setCursor(60, 210);
  CoreS3.Display.print("Shake to change emoji!");
}

// 绘制设置界面
void drawSettings() {
  CoreS3.Display.fillScreen(BLACK);
  
  // 顶部栏
  CoreS3.Display.fillRect(0, 0, 320, 30, 0x7BEF);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(1.5);
  CoreS3.Display.setCursor(10, 7);
  CoreS3.Display.print("Settings");
  
  // 返回按钮
  CoreS3.Display.fillCircle(300, 15, 10, RED);
  CoreS3.Display.setCursor(296, 8);
  CoreS3.Display.print("<");
  
  // 设置项
  CoreS3.Display.setTextSize(1);
  int y = 50;
  auto drawSetting = [&](const char* label, const char* value) {
    CoreS3.Display.drawFastHLine(10, y, 300, 0x632C);
    y += 10;
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setCursor(10, y);
    CoreS3.Display.print(label);
    CoreS3.Display.setCursor(200, y);
    CoreS3.Display.print(value);
    y += 25;
  };
  
  drawSetting("WiFi Status", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  drawSetting("IP Address", WiFi.localIP().toString().c_str());
  drawSetting("LLM Module", LLMModule.available() ? "Connected" : "Disconnected");
  drawSetting("Device Count", String(devices.size()).c_str());
  drawSetting("Version", "v1.0.0");
}

// 绘制表情脸
void drawEmojiFace(int emojiId) {
  int centerX = 160, centerY = 120;
  int faceSize = 100;
  
  // 脸
  CoreS3.Display.fillCircle(centerX, centerY, faceSize, 0xFFE0); // 黄色
  CoreS3.Display.drawCircle(centerX, centerY, faceSize, BLACK);
  
  switch(emojiId) {
    case 0: // 微笑
      // 眼睛
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
      // 嘴巴
      CoreS3.Display.drawArc(centerX, centerY + 10, 40, 30, 180, 360, BLACK);
      break;
      
    case 1: // 开心
      // 眼睛
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
      // 嘴巴
      CoreS3.Display.fillCircle(centerX, centerY + 10, 30, BLACK);
      CoreS3.Display.fillCircle(centerX, centerY, 30, 0xFFE0);
      break;
      
    case 2: // 惊讶
      // 眼睛
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 15, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 15, BLACK);
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 5, WHITE);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 5, WHITE);
      // 嘴巴
      CoreS3.Display.fillCircle(centerX, centerY + 20, 15, BLACK);
      break;
      
    case 3: // 难过
      // 眼睛
      CoreS3.Display.fillRoundRect(centerX - 40, centerY - 30, 20, 10, 5, BLACK);
      CoreS3.Display.fillRoundRect(centerX + 20, centerY - 30, 20, 10, 5, BLACK);
      // 嘴巴
      CoreS3.Display.drawArc(centerX, centerY + 30, 30, 20, 0, 180, BLACK);
      break;
  }
}

// 触摸处理
void handleTouch() {
  auto t = CoreS3.Touch.getDetail();
  if (t.state == m5::touch_state_t::touch_begin) {
    touchX = t.x;
    touchY = t.y;
    touchStartX = t.x;
    touchPressed = true;
    
    // 处理返回按钮
    if (currentApp != APP_HOME) {
      int dx = touchX - 300, dy = touchY - 15;
      if (dx*dx + dy*dy <= 100) {
        currentApp = APP_HOME;
        needRedraw = true;
        return;
      }
    }
    
    // 主界面点击APP卡片
    if (currentApp == APP_HOME) {
      int cardW = 130, cardH = 130;
      int spacing = 20;
      int startX = (320 - (cardW * 2 + spacing)) / 2;
      int cardY = 70;
      
      if (touchX >= startX && touchX <= startX + cardW && touchY >= cardY && touchY <= cardY + cardH) {
        currentApp = APP_CHAT;
        needRedraw = true;
      } else if (touchX >= startX + cardW + spacing && touchX <= startX + cardW * 2 + spacing && touchY >= cardY && touchY <= cardY + cardH) {
        currentApp = APP_CONTROL;
        needRedraw = true;
      } else if (touchX >= startX && touchX <= startX + cardW && touchY >= cardY + cardH + spacing && touchY <= cardY + cardH * 2 + spacing) {
        currentApp = APP_EMOJI;
        needRedraw = true;
      } else if (touchX >= startX + cardW + spacing && touchX <= startX + cardW * 2 + spacing && touchY >= cardY + cardH + spacing && touchY <= cardY + cardH * 2 + spacing) {
        currentApp = APP_SETTINGS;
        needRedraw = true;
      }
    }
    
    // 对话界面语音按钮
    if (currentApp == APP_CHAT && !isResponding) {
      int dx = touchX - 280, dy = touchY - 225;
      if (dx*dx + dy*dy <= 144) {
        processVoiceInput();
        needRedraw = true;
      }
    }
    
  } else if (t.state == m5::touch_state_t::touch_end) {
    touchPressed = false;
    
    // 滑动检测
    if (currentApp == APP_HOME && touchStartX != -1) {
      int deltaX = t.x - touchStartX;
      if (abs(deltaX) > SWIPE_THRESHOLD) {
        handleSwipe(deltaX);
      }
    }
    touchStartX = -1;
  }
}

// 滑动处理
void handleSwipe(int deltaX) {
  // 暂未实现多页滑动，预留扩展
  needRedraw = true;
}

// 处理语音输入
void processVoiceInput() {
  if (isResponding) return;
  
  // 显示正在录音
  CoreS3.Display.fillRect(0, 210, 320, 30, RED);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setCursor(10, 218);
  CoreS3.Display.print("Recording...");
  CoreS3.Speaker.tone(1000, 100);
  
  // 启动语音识别
  LLMModule.println("AT+ASR=START");
  String asrResult = "";
  uint32_t start = millis();
  while (millis() - start < 5000) {
    while (LLMModule.available()) {
      char c = LLMModule.read();
      asrResult += c;
      if (c == '\n') break;
    }
    if (asrResult.indexOf("ASR_RESULT") != -1) break;
    delay(10);
  }
  
  // 解析结果
  if (asrResult.indexOf("ASR_RESULT=") != -1) {
    int pos = asrResult.indexOf("=") + 1;
    String text = asrResult.substring(pos, asrResult.indexOf('\n', pos));
    text.trim();
    
    if (text.length() > 0) {
      // 添加到对话历史
      chatHistory.push_back({"user", text, millis()});
      if (chatHistory.size() > MAX_HISTORY) {
        chatHistory.erase(chatHistory.begin());
      }
      
      // 处理LLM请求
      isResponding = true;
      processLLMResponse();
    }
  }
  
  isResponding = false;
  needRedraw = true;
}

// 处理LLM响应
void processLLMResponse() {
  // 构建提示词
  String prompt = "";
  for (auto& msg : chatHistory) {
    prompt += msg.role + ": " + msg.content + "\n";
  }
  prompt += "assistant: ";
  
  // 发送到LLM模块
  LLMModule.printf("AT+CHAT=%s\n", prompt.c_str());
  
  // 接收响应
  String response = "";
  uint32_t start = millis();
  while (millis() - start < 10000) { // 10秒超时
    while (LLMModule.available()) {
      char c = LLMModule.read();
      response += c;
      if (c == '\n') break;
    }
    if (response.indexOf("CHAT_RESULT") != -1) break;
    delay(10);
  }
  
  // 解析响应
  if (response.indexOf("CHAT_RESULT=") != -1) {
    int pos = response.indexOf("=") + 1;
    String text = response.substring(pos, response.indexOf('\n', pos));
    text.trim();
    
    // 添加到历史
    chatHistory.push_back({"assistant", text, millis()});
    if (chatHistory.size() > MAX_HISTORY) {
      chatHistory.erase(chatHistory.begin());
    }
    
    // TTS播报
    sendTTS(text);
    
    // 广播到网页端
    broadcastMessage("chat", "{\"role\":\"assistant\",\"content\":\"" + text + "\"}");
  }
}

// 发送TTS
void sendTTS(String text) {
  LLMModule.printf("AT+TTS=%s\n", text.c_str());
  
  // 等待TTS完成
  uint32_t start = millis();
  while (millis() - start < 30000) { // 30秒超时
    while (LLMModule.available()) {
      String resp = LLMModule.readStringUntil('\n');
      if (resp.indexOf("TTS_DONE") != -1) return;
    }
    delay(10);
  }
}

// 更新传感器数据
void updateSensors() {
  CoreS3.Imu.getAccel(&accX, &accY, &accZ);
  CoreS3.Imu.getGyro(&gyroX, &gyroY, &gyroZ);
  
  checkShake();
}

// 检测摇晃
void checkShake() {
  if (millis() - lastShakeTime < SHAKE_COOLDOWN) return;
  
  float totalAcc = sqrt(accX*accX + accY*accY + accZ*accZ);
  if (totalAcc > 2.5) { // 摇晃阈值
    currentEmoji = (currentEmoji + 1) % 4;
    lastShakeTime = millis();
    CoreS3.Speaker.tone(800, 50);
    needRedraw = true;
  }
}

// Web服务器处理
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

void handleWebChat() {
  if (!server.hasArg("text")) {
    server.send(400, "application/json", "{\"error\":\"Missing text parameter\"}");
    return;
  }
  
  String text = server.arg("text");
  chatHistory.push_back({"user", text, millis()});
  if (chatHistory.size() > MAX_HISTORY) {
    chatHistory.erase(chatHistory.begin());
  }
  
  isResponding = true;
  processLLMResponse();
  isResponding = false;
  
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// WebSocket事件处理
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, msg);
    
    if (!error && doc["type"] == "chat") {
      String text = doc["payload"].as<String>();
      chatHistory.push_back({"user", text, millis()});
      if (chatHistory.size() > MAX_HISTORY) {
        chatHistory.erase(chatHistory.begin());
      }
      
      isResponding = true;
      processLLMResponse();
      isResponding = false;
      needRedraw = true;
    }
  }
}

// 广播消息到所有WebSocket客户端
void broadcastMessage(String type, String data) {
  String json = "{\"type\":\"" + type + "\",\"payload\":" + data + "}";
  webSocket.broadcastTXT(json.c_str());
}

// 绘制UI
void drawUI() {
  if (!needRedraw && millis() - lastDrawTime < DRAW_INTERVAL) return;
  
  switch(currentApp) {
    case APP_HOME: drawHome(); break;
    case APP_CHAT: drawChat(); break;
    case APP_CONTROL: drawControl(); break;
    case APP_EMOJI: drawEmoji(); break;
    case APP_SETTINGS: drawSettings(); break;
  }
  
  lastDrawTime = millis();
  needRedraw = false;
}

void setup() {
  initHardware();
  initNetwork();
  initLLMModule();
  
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
  
  needRedraw = true;
}

void loop() {
  CoreS3.update();
  server.handleClient();
  webSocket.loop();
  
  handleTouch();
  updateSensors();
  drawUI();
  
  // 处理M5按键（按住说话）
  if (CoreS3.BtnA.isPressed() && currentApp == APP_CHAT) {
    processVoiceInput();
  }
  
  delay(10);
}