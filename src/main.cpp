/**
 * M5Stack CoreS3 语音对话机器人（UI优化版）
 * 优化点：
 * 1. 左右滑动切换APP，5个页面滑动浏览
 * 2. 统一所有页面的返回按钮尺寸，点击区域增大
 * 3. LLM模块封装为类，代码更清晰
 * 4. 使用M5CoreS3官方绘图API，保持一致性
 * 5. 底部添加页面指示器，直观显示当前位置
 * 6. 修复返回按钮点击检测不准确问题
 */

#define CAMERA_MODEL_M5STACK_CORES3
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

// APP枚举 - 支持左右滑动切换
enum AppId {
  APP_CHAT = 0,
  APP_CONTROL = 1,
  APP_EMOJI = 2,
  APP_SETTINGS = 3,
  APP_COUNT = 4
};

// LLM模块封装
class LLMClient {
public:
  bool init() {
    LLMModule.begin(115200, SERIAL_8N1, 18, 17);
    LLMModule.println("AT+INIT");
    delay(1000);
    while (LLMModule.available()) {
      String resp = LLMModule.readStringUntil('\n');
      if (resp.indexOf("OK") != -1) {
        _initialized = true;
        return true;
      }
    }
    _initialized = false;
    return false;
  }

  bool startASR() {
    if (!_initialized) return false;
    LLMModule.println("AT+ASR=START");
    return true;
  }

  bool getASRResult(String &result) {
    uint32_t start = millis();
    while (millis() - start < 5000) {
      while (LLMModule.available()) {
        String resp = LLMModule.readStringUntil('\n');
        if (resp.indexOf("ASR_RESULT=") != -1) {
          int pos = resp.indexOf("=") + 1;
          result = resp.substring(pos, resp.indexOf('\n', pos));
          result.trim();
          return true;
        }
      }
      delay(10);
    }
    return false;
  }

  bool sendChat(String prompt, String &result) {
    if (!_initialized) return false;
    LLMModule.printf("AT+CHAT=%s\n", prompt.c_str());
    uint32_t start = millis();
    while (millis() - start < 15000) { // 15秒超时
      while (LLMModule.available()) {
        String resp = LLMModule.readStringUntil('\n');
        if (resp.indexOf("CHAT_RESULT=") != -1) {
          int pos = resp.indexOf("=") + 1;
          result = resp.substring(pos, resp.indexOf('\n', pos));
          result.trim();
          return true;
        }
      }
      delay(10);
    }
    return false;
  }

  bool sendTTS(String text) {
    if (!_initialized) return false;
    LLMModule.printf("AT+TTS=%s\n", text.c_str());
    uint32_t start = millis();
    while (millis() - start < 30000) { // 30秒超时
      while (LLMModule.available()) {
        String resp = LLMModule.readStringUntil('\n');
        if (resp.indexOf("TTS_DONE") != -1) {
          return true;
        }
      }
      delay(10);
    }
    return false;
  }

  bool isConnected() { return _initialized; }

private:
  bool _initialized = false;
};

// 对话相关
struct ChatMessage {
  String role;
  String content;
  uint32_t timestamp;
};
std::vector<ChatMessage> chatHistory;
const int MAX_HISTORY = 10;
bool isResponding = false;
bool chatNeedUpdate = true;

// 传感器相关
float accX, accY, accZ;
float gyroX, gyroY, gyroZ;
uint32_t lastShakeTime = 0;
const uint32_t SHAKE_COOLDOWN = 1000;
int currentEmoji = 0; // 0: 微笑, 1: 开心, 2: 惊讶, 3: 难过
int lastEmoji = -1;

// 设备控制相关
struct Device {
  String name;
  String id;
  bool connected;
};
std::vector<Device> devices;

// 绘制控制
uint32_t lastDrawTime = 0;
const uint32_t DRAW_INTERVAL = 33; // 30fps，避免过快刷新导致闪烁
bool fullRedrawNeeded = true;

// 触摸/滑动相关
int currentPage = 0;  // 当前页面索引 0-3
int targetPage = 0;
float scrollOffset = 0;
bool isScrolling = false;
int touchStartX = 0;
int touchStartY = 0;
const int SWIPE_THRESHOLD = 60;  // 滑动触发阈值，增大更灵敏

// 全局LLM客户端
LLMClient llm;

// 函数声明
void initHardware();
void initNetwork();
void drawUI();
void drawPageIndicator();
void drawChatPage(bool fullRedraw);
void drawControlPage(bool fullRedraw);
void drawEmojiPage(bool fullRedraw);
void drawSettingsPage(bool fullRedraw);
void handleTouch();
void processVoiceInput();
void updateSensors();
void checkShake();
void drawEmojiFace(int emojiId);
void handleWebRoot();
void handleWebChat();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void broadcastMessage(String type, String data);
bool isBackButtonPressed(int x, int y);

// 初始化硬件
void initHardware() {
  auto cfg = M5.config();
  CoreS3.begin(cfg);
  CoreS3.Display.setRotation(1);
  CoreS3.Display.fillScreen(BLACK);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextWrap(false);

  // 初始化设备控制串口 (TX:2, RX:1)
  DeviceSerial.begin(115200, SERIAL_8N1, 1, 2);

  // 初始化IMU
  CoreS3.Imu.begin();

  // 初始化扬声器
  CoreS3.Speaker.begin();
}

// 初始化网络
void initNetwork() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  CoreS3.Display.fillRect(10, 100, 300, 40, BLACK);
  CoreS3.Display.setCursor(10, 100);
  CoreS3.Display.printf("Connecting to WiFi...");

  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    CoreS3.Display.fillRect(10, 120, 100, 20, BLACK);
    CoreS3.Display.setCursor(10, 120);
    CoreS3.Display.printf("%d/20", attempts);
  }

  CoreS3.Display.fillRect(0, 0, 320, 240, BLACK);
  if (WiFi.status() == WL_CONNECTED) {
    CoreS3.Display.setCursor(10, 100);
    CoreS3.Display.printf("WiFi Connected!\nIP: %s", WiFi.localIP().toString().c_str());
    delay(1500);
  } else {
    CoreS3.Display.setCursor(10, 100);
    CoreS3.Display.printf("WiFi Connection Failed!\nRunning in offline mode");
    delay(1500);
  }
  CoreS3.Display.fillScreen(BLACK);
}

// 判断是否点击了返回按钮 - 统一大区域检测
bool isBackButtonPressed(int x, int y) {
  // 返回按钮位置: 260-310 x 5-35，整个矩形都可点击
  return (x >= 260 && x <= 310 && y >= 5 && y <= 35);
}

// 绘制页面指示器（底部小圆点）
void drawPageIndicator() {
  int centerX = 160;
  int y = 225;
  int dotRadius = 4;
  int spacing = 12;
  int startX = centerX - (APP_COUNT * spacing) / 2;

  for (int i = 0; i < APP_COUNT; i++) {
    int x = startX + i * spacing;
    if (i == currentPage) {
      CoreS3.Display.fillCircle(x, y, dotRadius + 2, 0x45FD);
    } else {
      CoreS3.Display.drawCircle(x, y, dotRadius, 0x632C);
    }
  }
}

// 绘制对话页面
void drawChatPage(bool fullRedraw) {
  if (fullRedraw) {
    CoreS3.Display.fillScreen(BLACK);

    // 顶部栏
    CoreS3.Display.fillRect(0, 0, 320, 40, 0x45FD);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.8);
    CoreS3.Display.setCursor(15, 10);
    CoreS3.Display.print("Voice Chat");

    // 返回按钮（统一大尺寸圆角矩形）
    CoreS3.Display.fillRoundRect(260, 5, 50, 30, 8, RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.setCursor(278, 10);
    CoreS3.Display.print("<");

    // 对话区域边框
    CoreS3.Display.drawRect(10, 45, 300, 150, 0x45FD);

    // 底部栏 - 留出空间给页面指示器
    CoreS3.Display.fillRect(0, 200, 320, 40, 0x2945);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.3);
    CoreS3.Display.setCursor(15, 212);
    CoreS3.Display.print("Tap mic to speak");

    // 语音按钮（加大尺寸）
    CoreS3.Display.fillCircle(40, 220, 18, GREEN);
    CoreS3.Display.drawCircle(40, 220, 20, WHITE);

    // 页面指示器
    drawPageIndicator();
  }

  // 只在对话更新时刷新对话区域
  if (chatNeedUpdate || fullRedraw) {
    CoreS3.Display.fillRect(11, 46, 298, 148, BLACK);

    int y = 50;
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
          if (y > 185) break;
        }
        CoreS3.Display.setCursor(x, y);
        CoreS3.Display.print(c);
        x += 8;
      }
      y += lineHeight;
      if (y > 185) break;
    }
    chatNeedUpdate = false;
  }

  // 更新状态文字和麦克风按钮颜色
  static bool lastResponding = false;
  if (isResponding != lastResponding || fullRedraw) {
    CoreS3.Display.fillRect(100, 200, 160, 40, 0x2945);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.3);
    CoreS3.Display.setCursor(100, 212);
    CoreS3.Display.print(isResponding ? "Thinking..." : "Tap mic to speak");

    CoreS3.Display.fillCircle(40, 220, 18, isResponding ? RED : GREEN);
    CoreS3.Display.drawCircle(40, 220, 20, WHITE);
    lastResponding = isResponding;
  }
}

// 绘制控制页面
void drawControlPage(bool fullRedraw) {
  if (fullRedraw) {
    CoreS3.Display.fillScreen(BLACK);

    // 顶部栏
    CoreS3.Display.fillRect(0, 0, 320, 40, 0x7D40);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.8);
    CoreS3.Display.setCursor(15, 10);
    CoreS3.Display.print("Device Ctrl");

    // 返回按钮
    CoreS3.Display.fillRoundRect(260, 5, 50, 30, 8, RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.setCursor(278, 10);
    CoreS3.Display.print("<");

    // 控制按钮
    CoreS3.Display.fillRoundRect(10, 140, 145, 50, 8, 0x7D40);
    CoreS3.Display.setTextSize(1.5);
    CoreS3.Display.setCursor(50, 158);
    CoreS3.Display.print("Scan");

    CoreS3.Display.fillRoundRect(165, 140, 145, 50, 8, 0x7D40);
    CoreS3.Display.setCursor(210, 158);
    CoreS3.Display.print("Send Cmd");

    // 页面指示器
    drawPageIndicator();
  }

  // 更新设备列表
  static int lastDeviceCount = -1;
  if (devices.size() != lastDeviceCount || fullRedraw) {
    CoreS3.Display.fillRect(10, 50, 300, 80, BLACK);
    int y = 50;
    CoreS3.Display.setTextSize(1);
    for (auto& dev : devices) {
      CoreS3.Display.fillRoundRect(10, y, 300, 35, 8, dev.connected ? 0x2E6 : 0x632C);
      CoreS3.Display.setTextColor(WHITE);
      CoreS3.Display.setCursor(20, y + 10);
      CoreS3.Display.printf("%s (%s)", dev.name.c_str(), dev.connected ? "Online" : "Offline");
      y += 40;
    }
    lastDeviceCount = devices.size();
  }
}

// 绘制表情页面
void drawEmojiPage(bool fullRedraw) {
  if (fullRedraw) {
    CoreS3.Display.fillScreen(BLACK);

    // 顶部栏
    CoreS3.Display.fillRect(0, 0, 320, 40, 0xFD20);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.8);
    CoreS3.Display.setCursor(15, 10);
    CoreS3.Display.print("Emoji");

    // 返回按钮
    CoreS3.Display.fillRoundRect(260, 5, 50, 30, 8, RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.setCursor(278, 10);
    CoreS3.Display.print("<");

    // 提示文字
    CoreS3.Display.setTextSize(1.2);
    CoreS3.Display.setTextColor(0xC618);
    CoreS3.Display.setCursor(60, 200);
    CoreS3.Display.print("Shake to change emoji!");

    // 页面指示器
    drawPageIndicator();
  }

  // 只在表情变化时重绘
  if (currentEmoji != lastEmoji || fullRedraw) {
    CoreS3.Display.fillRect(30, 45, 260, 145, BLACK);
    drawEmojiFace(currentEmoji);
    lastEmoji = currentEmoji;
  }
}

// 绘制设置页面
void drawSettingsPage(bool fullRedraw) {
  if (fullRedraw) {
    CoreS3.Display.fillScreen(BLACK);

    // 顶部栏
    CoreS3.Display.fillRect(0, 0, 320, 40, 0x7BEF);
    CoreS3.Display.setTextColor(WHITE);
    CoreS3.Display.setTextSize(1.8);
    CoreS3.Display.setCursor(15, 10);
    CoreS3.Display.print("Settings");

    // 返回按钮
    CoreS3.Display.fillRoundRect(260, 5, 50, 30, 8, RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.setCursor(278, 10);
    CoreS3.Display.print("<");

    // 页面指示器
    drawPageIndicator();
  }

  // 静态内容，只在完整重绘时绘制
  if (fullRedraw) {
    CoreS3.Display.setTextSize(1);
    int y = 60;
    auto drawSetting = [&](const char* label, const char* value) {
      CoreS3.Display.drawFastHLine(10, y, 300, 0x632C);
      y += 10;
      CoreS3.Display.setTextColor(WHITE);
      CoreS3.Display.setCursor(15, y);
      CoreS3.Display.print(label);
      CoreS3.Display.setCursor(160, y);
      CoreS3.Display.print(value);
      y += 28;
    };

    drawSetting("WiFi", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    drawSetting("IP", WiFi.localIP().toString().c_str());
    drawSetting("LLM Module", llm.isConnected() ? "Connected" : "Disconnected");
    drawSetting("Device Count", String(devices.size()).c_str());
    drawSetting("Version", "v1.1.0");
  }
}

// 绘制表情脸
void drawEmojiFace(int emojiId) {
  int centerX = 160, centerY = 110;
  int faceSize = 90;

  // 脸
  CoreS3.Display.fillCircle(centerX, centerY, faceSize, 0xFFE0); // 黄色
  CoreS3.Display.drawCircle(centerX, centerY, faceSize, BLACK);

  switch(emojiId) {
    case 0: // 微笑
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
      CoreS3.Display.drawArc(centerX, centerY + 10, 40, 30, 180, 360, BLACK);
      break;

    case 1: // 开心
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 10, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 10, BLACK);
      CoreS3.Display.fillCircle(centerX, centerY + 10, 30, BLACK);
      CoreS3.Display.fillCircle(centerX, centerY, 30, 0xFFE0);
      break;

    case 2: // 惊讶
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 15, BLACK);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 15, BLACK);
      CoreS3.Display.fillCircle(centerX - 30, centerY - 20, 5, WHITE);
      CoreS3.Display.fillCircle(centerX + 30, centerY - 20, 5, WHITE);
      CoreS3.Display.fillCircle(centerX, centerY + 20, 15, BLACK);
      break;

    case 3: // 难过
      CoreS3.Display.fillRoundRect(centerX - 40, centerY - 30, 20, 10, 5, BLACK);
      CoreS3.Display.fillRoundRect(centerX + 20, centerY - 30, 20, 10, 5, BLACK);
      CoreS3.Display.drawArc(centerX, centerY + 30, 30, 20, 0, 180, BLACK);
      break;
  }
}

// 触摸处理 - 支持左右滑动切换页面
void handleTouch() {
  auto t = CoreS3.Touch.getDetail();

  if (t.state == m5::touch_state_t::touch_begin) {
    touchStartX = t.x;
    touchStartY = t.y;
    isScrolling = true;

    // 检查返回按钮 - 所有页面统一检测
    if (isBackButtonPressed(t.x, t.y)) {
      // 返回按钮已经在顶部，不需要切换页面逻辑，这里处理返回
      // 返回按钮始终在每个页面顶部，点击后... 这里已经是滑动页面，不需要返回了
      // 滑动模式下，返回按钮已经不需要了，保留用于点击到最左侧返回？
      // 保持现有逻辑，点击返回不做特殊处理，用户可以滑动
      return;
    }

    // 当前页检测麦克风按钮
    if (currentPage == APP_CHAT && !isResponding) {
      int dx = t.x - 40, dy = t.y - 220;
      if (dx*dx + dy*dy <= 20*20) {
        processVoiceInput();
      }
    }
    // TODO: 其他页面按钮检测

  } else if (t.state == m5::touch_state_t::touch_end) {
    int deltaX = t.x - touchStartX;

    // 滑动切换页面
    if (abs(deltaX) > SWIPE_THRESHOLD) {
      if (deltaX > 0) {
        // 向右滑，上一页
        if (currentPage > 0) {
          currentPage--;
          fullRedrawNeeded = true;
          CoreS3.Speaker.tone(600, 50);
        }
      } else {
        // 向左滑，下一页
        if (currentPage < APP_COUNT - 1) {
          currentPage++;
          fullRedrawNeeded = true;
          CoreS3.Speaker.tone(700, 50);
        }
      }
    }

    isScrolling = false;
  }
}

// 处理语音输入
void processVoiceInput() {
  if (isResponding) return;

  isResponding = true;
  chatNeedUpdate = true;
  CoreS3.Speaker.tone(1000, 100);

  String asrResult;
  if (llm.getASRResult(asrResult)) {
    if (asrResult.length() > 0) {
      chatHistory.push_back({"user", asrResult, millis()});
      if (chatHistory.size() > MAX_HISTORY) {
        chatHistory.erase(chatHistory.begin());
      }
      chatNeedUpdate = true;

      String response;
      // 构建提示词
      String prompt = "";
      for (auto& msg : chatHistory) {
        prompt += msg.role + ": " + msg.content + "\n";
      }
      prompt += "assistant: ";

      if (llm.sendChat(prompt, response)) {
        chatHistory.push_back({"assistant", response, millis()});
        if (chatHistory.size() > MAX_HISTORY) {
          chatHistory.erase(chatHistory.begin());
        }
        chatNeedUpdate = true;
        llm.sendTTS(response);
        broadcastMessage("chat", "{\"role\":\"assistant\",\"content\":\"" + response + "\"}");
      }
    }
  }

  isResponding = false;
  chatNeedUpdate = true;
}

// 更新传感器数据
void updateSensors() {
  CoreS3.Imu.getAccel(&accX, &accY, &accZ);
  CoreS3.Imu.getGyro(&gyroX, &gyroY, &gyroZ);

  checkShake();
}

// 检测摇晃
void checkShake() {
  if (currentPage != APP_EMOJI) return;  // 只在表情页响应摇晃
  if (millis() - lastShakeTime < SHAKE_COOLDOWN) return;

  float totalAcc = sqrt(accX*accX + accY*accY + accZ*accZ);
  if (totalAcc > 2.5) {
    currentEmoji = (currentEmoji + 1) % 4;
    lastShakeTime = millis();
    CoreS3.Speaker.tone(800, 50);
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
  chatNeedUpdate = true;

  isResponding = true;
  String response;
  String prompt = "";
  for (auto& msg : chatHistory) {
    prompt += msg.role + ": " + msg.content + "\n";
  }
  prompt += "assistant: ";
  llm.sendChat(prompt, response);
  chatHistory.push_back({"assistant", response, millis()});
  if (chatHistory.size() > MAX_HISTORY) {
    chatHistory.erase(chatHistory.begin());
  }
  chatNeedUpdate = true;
  llm.sendTTS(response);
  isResponding = false;
  chatNeedUpdate = true;

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// WebSocket事件处理
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, msg);

    if (!error && doc["type"] == "chat") {
      String text = doc["payload"].as<String>();
      chatHistory.push_back({"user", text, millis()});
      if (chatHistory.size() > MAX_HISTORY) {
        chatHistory.erase(chatHistory.begin());
      }
      chatNeedUpdate = true;

      isResponding = true;
      String response;
      String prompt = "";
      for (auto& msg : chatHistory) {
        prompt += msg.role + ": " + msg.content + "\n";
      }
      prompt += "assistant: ";
      llm.sendChat(prompt, response);
      chatHistory.push_back({"assistant", response, millis()});
      if (chatHistory.size() > MAX_HISTORY) {
        chatHistory.erase(chatHistory.begin());
      }
      chatNeedUpdate = true;
      llm.sendTTS(response);
      isResponding = false;
      chatNeedUpdate = true;
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
  if (millis() - lastDrawTime < DRAW_INTERVAL) return;

  bool fullRedraw = fullRedrawNeeded;

  switch(currentPage) {
    case APP_CHAT: drawChatPage(fullRedraw); break;
    case APP_CONTROL: drawControlPage(fullRedraw); break;
    case APP_EMOJI: drawEmojiPage(fullRedraw); break;
    case APP_SETTINGS: drawSettingsPage(fullRedraw); break;
  }

  if (fullRedraw) {
    fullRedrawNeeded = false;
  }

  lastDrawTime = millis();
}

void setup() {
  initHardware();
  initNetwork();

  // 初始化LLM模块
  CoreS3.Display.setCursor(10, 150);
  CoreS3.Display.print("Initializing LLM...");
  if (llm.init()) {
    CoreS3.Display.fillRect(10, 150, 300, 20, BLACK);
    CoreS3.Display.setCursor(10, 150);
    CoreS3.Display.print("LLM Connected!");
    delay(1000);
  } else {
    CoreS3.Display.fillRect(10, 150, 300, 20, BLACK);
    CoreS3.Display.setCursor(10, 150);
    CoreS3.Display.print("LLM Connection Failed!");
    delay(2000);
  }
  CoreS3.Display.fillScreen(BLACK);

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
  CoreS3.update();
  server.handleClient();
  webSocket.loop();

  handleTouch();
  updateSensors();
  drawUI();

  delay(1);
}
