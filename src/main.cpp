/**
 * M5Stack CoreS3 离线语音对话机器人 v3.0
 *
 * 优化点：
 * - 首页App桌面风格，白色背景，图标+文字卡片
 * - 支持左右滑动切换整个桌面
 * - 点击APP卡片有缩放动画反馈
 * - 使用FreeRTOS双核处理，LLM在核心0，UI在核心1
 * - 基于官方 M5ModuleLLM + M5Unified + M5GFX
 */

// 使用 M5Unified 推荐驱动
#include <M5Unified.h>
#include <M5ModuleLLM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

// ========== 配置 ==========
const char* WIFI_SSID     = "你的WiFi";
const char* WIFI_PASSWORD = "你的WiFi密码";
const char* DEVICE_NAME   = "M5-LLM-Robot";

// ========== 应用定义 ==========
#define MAX_APPS 4

struct App {
    const char* name;
    const char* icon;
    uint16_t    color;
};

const App apps[MAX_APPS] = {
    {"Chat",    "💬", 0x45FD},   // 蓝
    {"Control","🎮", 0x7D40},   // 绿
    {"Emoji",  "😊", 0xFD20},   // 黄
    {"Settings","⚙️",0x7BEF}    // 紫
};

// ========== 全局对象 ==========
M5ModuleLLM     module_llm;
WebServer       server(80);
WebSocketsServer webSocket(81);
HardwareSerial  deviceSerial(1);  // 外部设备串口

// ========== APP状态 ==========
int   currentAppPage = 0;         // 当前页 (0-0: 第一页4个app)
float scrollOffsetX = 0;          // 滚动偏移
bool  isScrolling = false;
int   touchStartX = 0;
int   touchStartY = 0;
const int SWIPE_THRESHOLD = 50;
const int SCREEN_WIDTH  = 320;
const int SCREEN_HEIGHT = 240;

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
const uint32_t DRAW_INTERVAL = 16;  // ~60fps
bool fullRedrawNeeded = true;

// ========== RTOS ==========
TaskHandle_t llmTaskHandle;
xQueueHandle_t llmCommandQueue;

enum LlmCommandType {
    CMD_ASR_START,
    CMD_CHAT_INFERENCE
};

struct LlmCommand {
    LlmCommandType type;
    String data;
};

// ========== 回调函数 ==========
void onLlmResult(String data, bool isFinish, int index);

// ========== 函数声明 ==========
void initHardware();
void initNetwork();
void initLlmModule();
void drawUI();
void drawHomeLauncher();
void drawAppIcon(int index, int offsetX, bool isPressed = false);
void drawChatApp();
void drawControlApp();
void drawEmojiApp();
void drawSettingsApp();
void handleTouch();
void startAsrInference();
void processLlmInference(String asrText);
void drawEmojiFace(int emojiId);
void handleWebRoot();
void handleWebChat();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void broadcastMessage(String type, String data);
void llmTask(void *arg);
void updateSensors();

// ========== 颜色定义 ==========
const uint16_t BG_COLOR     = M5.Display.color565(250, 250, 250);  // 背景白色
const uint16_t TEXT_COLOR   = M5.Display.color565(30, 30, 30);        // 文字深色
const uint16_t CARD_BORDER  = M5.Display.color565(220, 220, 220);    // 卡片边框

// ========== 初始化硬件 ==========
void initHardware() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(BG_COLOR);
    M5.Display.setTextColor(TEXT_COLOR);
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
    M5.Display.fillRect(10, 100, 300, 40, BG_COLOR);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setCursor(10, 100);
    M5.Display.printf("Connecting to WiFi...");
    M5.update();

    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
        M5.Display.fillRect(10, 120, 100, 20, BG_COLOR);
        M5.Display.setCursor(10, 120);
        M5.Display.printf("%d/20", attempts);
        M5.update();
    }

    M5.Display.fillScreen(BG_COLOR);
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.setCursor(10, 100);
        M5.Display.printf("WiFi Connected!\nIP: %s", WiFi.localIP().toString().c_str());
        delay(1500);
    } else {
        M5.Display.setCursor(10, 100);
        M5.Display.printf("WiFi Connection Failed!\nRunning in offline mode");
        delay(1500);
    }
    M5.Display.fillScreen(BG_COLOR);
}

// ========== 初始化LLM模块 ==========
void initLlmModule() {
    M5.Display.setCursor(10, 150);
    M5.Display.print("Initializing LLM Module...");
    M5.update();

    // begin 使用串口2
    module_llm.begin(&Serial2);
    Serial2.begin(115200, SERIAL_8N1, 18, 17);  // RX:18, TX:17

    // 创建命令队列
    llmCommandQueue = xQueueCreate(10, sizeof(LlmCommand));

    // 创建LLM任务到核心0
    xTaskCreatePinnedToCore(llmTask, "llmTask", 8192, NULL, 2, &llmTaskHandle, 0);

    // 检查连接
    if (!module_llm.checkConnection()) {
        M5.Display.fillRect(10, 150, 300, 20, BG_COLOR);
        M5.Display.setCursor(10, 150);
        M5.Display.print("LLM Module Not Connected!");
        delay(2000);
        M5.Display.fillScreen(BG_COLOR);
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

    M5.Display.fillRect(10, 150, 300, 20, BG_COLOR);
    M5.Display.setCursor(10, 150);
    M5.Display.print("LLM Module Ready!");
    delay(1000);
    M5.Display.fillScreen(BG_COLOR);
}

// ========== 绘制桌面 ==========
void drawHomeLauncher() {
    // 背景白色
    M5.Display.fillScreen(BG_COLOR);

    // 标题
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setCursor(15, 15);
    M5.Display.print(DEVICE_NAME);

    // 计算网格布局 2x2
    int cardW = 130;
    int cardH = 90;
    int spacing = 20;
    int startX = (SCREEN_WIDTH - (cardW * 2 + spacing)) / 2;
    int startY = 50;

    for (int i = 0; i < MAX_APPS; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = startX + col * (cardW + spacing) + (int)scrollOffsetX;
        int y = startY + row * (cardH + spacing);
        drawAppIcon(i, x);
    }

    // 页面指示器 - 底部
    int centerX = SCREEN_WIDTH / 2;
    int y = 220;
    int dotR = 4;
    int spacing = 12;
    int start = centerX - spacing / 2;
    M5.Display.drawCircle(start, y, dotR, CARD_BORDER);
    M5.Display.fillCircle(start, y, dotR + (currentAppPage == 0 ? 2 : 0), currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.drawCircle(start + spacing, y, dotR, CARD_BORDER);
    M5.Display.fillCircle(start + spacing, y, dotR + (currentAppPage == 1 ? 2 : 0), currentAppPage == 1 ? apps[2].color : CARD_BORDER);
}

// ========== 绘制单个APP图标 ==========
void drawAppIcon(int index, int offsetX, bool isPressed) {
    int cardW = 130;
    int cardH = 90;
    int cornerR = 12;

    int col = index % 2;
    int row = index / 2;
    int spacing = 20;
    int startX = (SCREEN_WIDTH - (cardW * 2 + spacing)) / 2;
    int startY = 50;
    int spacingY = 20;

    int x = startX + col * (cardW + spacing) + offsetX;
    int y = startY + row * (cardH + spacingY);

    // 按压缩放动画 - 缩小一点表示按下
    float scale = isPressed ? 0.95f : 1.0f;
    int drawW = cardW * scale;
    int drawH = cardH * scale;
    int drawX = x + (cardW - drawW) / 2;
    int drawY = y + (cardH - drawH) / 2;

    // 阴影
    M5.Display.fillRoundRect(drawX + 2, drawY + 2, drawW, drawH, cornerR, M5.Display.color565(200, 200, 200));
    // 卡片背景
    M5.Display.fillRoundRect(drawX, drawY, drawW, drawH, cornerR, apps[index].color);
    // 图标
    M5.Display.setTextSize(4);
    M5.Display.setCursor(drawX + (drawW - 48) / 2, drawY + 15);
    M5.Display.print(apps[index].icon);
    // 名称
    M5.Display.setTextSize(1.5);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(drawX + (drawW - 6*strlen(apps[index].name)) / 2, drawY + 60);
    M5.Display.print(apps[index].name);
}

// ========== 绘制对话应用 ==========
void drawChatApp() {
    static int lastResponding = -1;

    // 顶部栏
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, 45, apps[0].color);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1.8);
    M5.Display.setCursor(15, 12);
    M5.Display.print(apps[0].name);

    // 对话区域
    M5.Display.drawRect(5, 48, SCREEN_WIDTH - 10, 135, CARD_BORDER);
    M5.Display.fillRect(6, 49, SCREEN_WIDTH - 12, 133, WHITE);

    // 底部栏
    M5.Display.fillRect(0, 188, SCREEN_WIDTH, 52, M5.Display.color565(245, 245, 245));

    // 状态文字
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setTextSize(1.3);
    M5.Display.setCursor(70, 205);
    if (lastResponding != (int)isResponding || fullRedrawNeeded) {
        M5.Display.fillRect(60, 190, 200, 48, M5.Display.color565(245, 245, 245));
        M5.Display.setCursor(70, 205);
        M5.Display.print(isResponding ? "Thinking..." : "Tap mic to speak");
        lastResponding = isResponding;
    }

    // 麦克风按钮
    M5.Display.fillCircle(40, 210, 20, isResponding ? RED : GREEN);
    M5.Display.drawCircle(40, 210, 22, WHITE);

    // 页面指示器
    int centerX = SCREEN_WIDTH / 2;
    int y = 226;
    int dotR = 3;
    int spacing = 12;
    int start = centerX - spacing / 2;
    M5.Display.fillCircle(start, y, dotR + 2, currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.drawCircle(start + spacing, y, dotR, currentAppPage == 1 ? apps[2].color : CARD_BORDER);

    // 更新对话内容
    if (chatNeedUpdate || fullRedrawNeeded) {
        M5.Display.fillRect(8, 51, SCREEN_WIDTH - 16, 130, WHITE);

        int y = 60;
        int lineHeight = 16;
        M5.Display.setTextSize(1);
        int startIdx = max(0, (int)chatHistory.size() - 3);
        for (int i = startIdx; i < chatHistory.size(); i++) {
            auto& msg = chatHistory[i];
            M5.Display.setTextColor(msg.role == "user" ? apps[0].color : TEXT_COLOR);
            M5.Display.setCursor(15, y);
            String prefix = msg.role == "user" ? "You: " : "Bot: ";
            M5.Display.print(prefix);

            // 自动换行
            String content = msg.content;
            int x = 15 + 30;
            for (char c : content) {
                if (x > SCREEN_WIDTH - 20) {
                    x = 15;
                    y += lineHeight;
                    if (y > 165) break;
                }
                M5.Display.setCursor(x, y);
                M5.Display.print(c);
                x += 8;
            }
            y += lineHeight;
            if (y > 165) break;
        }
        chatNeedUpdate = false;
    }
}

// ========== 绘制控制应用 ==========
void drawControlApp() {
    // 顶部栏
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, 45, apps[1].color);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1.8);
    M5.Display.setCursor(15, 12);
    M5.Display.print(apps[1].name);

    // 控制按钮
    M5.Display.fillRoundRect(10, 60, SCREEN_WIDTH/2 - 15, 50, 8, apps[1].color);
    M5.Display.setTextSize(1.5);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(50, 82);
    M5.Display.print("Scan");

    M5.Display.fillRoundRect(SCREEN_WIDTH/2 + 5, 60, SCREEN_WIDTH/2 - 15, 50, 8, apps[1].color);
    M5.Display.setCursor(SCREEN_WIDTH/2 + 40, 82);
    M5.Display.print("Send Cmd");

    // 设备列表
    M5.Display.drawRect(10, 125, SCREEN_WIDTH - 20, 70, CARD_BORDER);
    M5.Display.fillRect(11, 126, SCREEN_WIDTH - 22, 68, WHITE);

    static int lastDeviceCount = -1;
    if (devices.size() != lastDeviceCount || fullRedrawNeeded) {
        M5.Display.fillRect(11, 126, SCREEN_WIDTH - 22, 68, WHITE);
        int y = 135;
        M5.Display.setTextSize(1);
        for (auto& dev : devices) {
            M5.Display.fillRoundRect(15, y - 5, SCREEN_WIDTH - 30, 28, 6, dev.connected ? M5.Display.color565(70, 200, 70) : M5.Display.color565(200, 200, 70));
            M5.Display.setTextColor(WHITE);
            M5.Display.setCursor(25, y + 3);
            M5.Display.printf("%s (%s)", dev.name.c_str(), dev.connected ? "Online" : "Offline");
            y += 32;
        }
        lastDeviceCount = devices.size();
    }

    // 页面指示器
    int centerX = SCREEN_WIDTH / 2;
    int y = 226;
    int dotR = 3;
    int spacing = 12;
    int start = centerX - spacing / 2;
    M5.Display.drawCircle(start, y, dotR, currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.fillCircle(start + spacing, y, dotR + 2, currentAppPage == 1 ? apps[2].color : CARD_BORDER);
}

// ========== 绘制表情应用 ==========
void drawEmojiApp() {
    // 顶部栏
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, 45, apps[2].color);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1.8);
    M5.Display.setCursor(15, 12);
    M5.Display.print(apps[2].name);

    // 提示文字
    M5.Display.setTextSize(1.2);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100));
    M5.Display.setCursor(60, 210);
    M5.Display.print("Shake to change emoji!");

    // 表情区域
    if (currentEmoji != lastEmoji || fullRedrawNeeded) {
        M5.Display.fillRect(30, 48, 260, 150, WHITE);
        drawEmojiFace(currentEmoji);
        lastEmoji = currentEmoji;
    }

    // 页面指示器
    int centerX = SCREEN_WIDTH / 2;
    int y = 226;
    int dotR = 3;
    int spacing = 12;
    int start = centerX - spacing / 2;
    M5.Display.drawCircle(start, y, dotR, currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.fillCircle(start + spacing, y, dotR + 2, currentAppPage == 1 ? apps[2].color : CARD_BORDER);
}

// ========== 绘制表情 ==========
void drawEmojiFace(int emojiId) {
    int centerX = 160, centerY = 120;
    int faceSize = 80;

    // 脸 - 黄色
    M5.Display.fillCircle(centerX, centerY, faceSize, 0xFFE0);
    M5.Display.drawCircle(centerX, centerY, faceSize, TEXT_COLOR);

    switch(emojiId) {
        case 0: // 微笑
            M5.Display.fillCircle(centerX - 28, centerY - 20, 10, TEXT_COLOR);
            M5.Display.fillCircle(centerX + 28, centerY - 20, 10, TEXT_COLOR);
            M5.Display.drawArc(centerX, centerY + 10, 38, 28, 180, 360, TEXT_COLOR);
            break;

        case 1: // 开心
            M5.Display.fillCircle(centerX - 28, centerY - 20, 10, TEXT_COLOR);
            M5.Display.fillCircle(centerX + 28, centerY - 20, 10, TEXT_COLOR);
            M5.Display.fillCircle(centerX, centerY + 10, 28, TEXT_COLOR);
            M5.Display.fillCircle(centerX, centerY, 28, 0xFFE0);
            break;

        case 2: // 惊讶
            M5.Display.fillCircle(centerX - 28, centerY - 20, 14, TEXT_COLOR);
            M5.Display.fillCircle(centerX + 28, centerY - 20, 14, TEXT_COLOR);
            M5.Display.fillCircle(centerX - 28, centerY - 20, 5, WHITE);
            M5.Display.fillCircle(centerX + 28, centerY - 20, 5, WHITE);
            M5.Display.fillCircle(centerX, centerY + 20, 15, TEXT_COLOR);
            break;

        case 3: // 难过
            M5.Display.fillRoundRect(centerX - 38, centerY - 28, 20, 10, 5, TEXT_COLOR);
            M5.Display.fillRoundRect(centerX + 18, centerY - 28, 20, 10, 5, TEXT_COLOR);
            M5.Display.drawArc(centerX, centerY + 28, 28, 18, 0, 180, TEXT_COLOR);
            break;
    }
}

// ========== 绘制设置应用 ==========
void drawSettingsApp() {
    // 顶部栏
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, 45, apps[3].color);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1.8);
    M5.Display.setCursor(15, 12);
    M5.Display.print(apps[3].name);

    if (fullRedrawNeeded) {
        M5.Display.fillScreen(WHITE);
        M5.Display.setTextSize(1);
        int y = 60;
        auto drawSetting = [&](const char* label, const char* value) {
            M5.Display.drawFastHLine(10, y, 300, CARD_BORDER);
            y += 8;
            M5.Display.setTextColor(TEXT_COLOR);
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
        drawSetting("Version", "v3.0.0");
    }

    // 页面指示器
    int centerX = SCREEN_WIDTH / 2;
    int y = 226;
    int dotR = 3;
    int spacing = 12;
    int start = centerX - spacing / 2;
    M5.Display.drawCircle(start, y, dotR, currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.fillCircle(start + spacing, y, dotR + 2, currentAppPage == 1 ? apps[2].color : CARD_BORDER);
}

// ========== 绘制UI主入口 ==========
void drawUI() {
    if (millis() - lastDrawTime < DRAW_INTERVAL) return;

    if (currentAppPage == 0) {
        drawHomeLauncher();
    } else {
        switch(currentAppPage - 1) {
            case 0: drawChatApp(); break;
            case 1: drawControlApp(); break;
            case 2: drawEmojiApp(); break;
            case 3: drawSettingsApp(); break;
        }
    }

    if (fullRedrawNeeded) {
        fullRedrawNeeded = false;
    }

    lastDrawTime = millis();
}

// ========== 触摸处理 ==========
void handleTouch() {
    auto t = M5.Touch.getDetail();

    if (t.state == m5::touch_state_t::touch_begin) {
        touchStartX = t.x;
        touchStartY = t.y;
        isScrolling = true;

        // 桌面点击检测app
        if (currentAppPage == 0) {
            int cardW = 130;
            int cardH = 90;
            int spacing = 20;
            int spacingY = 20;
            int startX = (SCREEN_WIDTH - (cardW * 2 + spacing)) / 2;
            int startY = 50;

            for (int i = 0; i < MAX_APPS; i++) {
                int col = i % 2;
                int row = i / 2;
                int x = startX + col * (cardW + spacing);
                int y = startY + row * (cardH + spacingY);
                if (t.x >= x && t.x <= x + cardW && t.y >= y && t.y <= y + cardH) {
                    // 按压动画 - 重绘按压状态
                    drawHomeLauncher();
                    drawAppIcon(i, 0, true);
                    M5.Display.display();
                    delay(80);
                    // 点击进入app
                    currentAppPage = i + 1;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(600, 60);
                    return;
                }
            }
        }
        // 对话页麦克风检测
        else if ((currentAppPage - 1) == 0 && !isResponding) {
            int dx = t.x - 40, dy = t.y - 210;
            if (dx*dx + dy*dy <= 22*22) {
                M5.Speaker.tone(1000, 80);
                LlmCommand cmd;
                cmd.type = CMD_ASR_START;
                xQueueSend(llmCommandQueue, &cmd, 0);
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
                if (currentAppPage > 0) {
                    currentAppPage--;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(600, 50);
                }
            } else {
                // 向左滑 - 下一页
                if (currentAppPage < MAX_APPS) {
                    currentAppPage++;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(700, 50);
                }
            }
        }
        isScrolling = false;
    }
}

// ========== LLM任务 - 运行在核心0 ==========
void llmTask(void *arg) {
    LlmCommand cmd;
    for(;;) {
        if (xQueueReceive(llmCommandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch(cmd.type) {
                case CMD_ASR_START:
                    isResponding = true;
                    chatNeedUpdate = true;
                    module_llm.tts.inference(workIds.tts, "请稍等，正在听");
                    // 等待ASR结果
                    String asrResult;
                    if (module_llm.asr.getResult(&asrResult)) {
                        processLlmInference(asrResult);
                    } else {
                        isResponding = false;
                    }
                    break;

                case CMD_CHAT_INFERENCE:
                    processLlmInference(cmd.data);
                    break;
            }
        }
        module_llm.update();
    }
}

// ========== 开始ASR ==========
void startAsrInference() {
    // 已经放到RTOS任务处理
}

// ========== LLM结果回调 ==========
void onLlmResult(String data, bool isFinish, int index) {
    currentResponse += data;
    if (isFinish) {
        chatHistory.push_back({"assistant", currentResponse});
        if (chatHistory.size() > MAX_CHAT_HISTORY) {
            chatHistory.erase(chatHistory.begin());
        }
        chatNeedUpdate = true;

        module_llm.tts.inference(workIds.tts, currentResponse);
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

    chatHistory.push_back({"user", asrText});
    if (chatHistory.size() > MAX_CHAT_HISTORY) {
        chatHistory.erase(chatHistory.begin());
    }
    chatNeedUpdate = true;
    currentResponse = "";
    isResponding = true;

    String prompt = "";
    for (auto& msg : chatHistory) {
        prompt += msg.role + ": " + msg.content + "\n";
    }
    prompt += "assistant: ";

    module_llm.llm.inference(workIds.llm, prompt);
}

// ========== 更新传感器 ==========
void updateSensors() {
    M5.Imu.getAccel(&accX, &accY, &accZ);
    M5.Imu.getGyro(&gyroX, &gyroY, &gyroZ);

    // 只在表情页检测摇晃
    if (currentAppPage == 1 && (currentAppPage - 1) == 2) {
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
  <meta