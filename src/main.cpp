/**
 * M5Stack CoreS3 离线语音对话机器人 v3.0
 *
 * 功能特点：
 * - 初始化启动界面，带旋转加载动画
 * - WiFi配置持久化存储（Preferences），配置一次无需重新配置
 * - 检查传感器和LLM Module连接状态
 * - 白色背景，支持中文字体
 * - 点击有缩放动画反馈
 * - 预留蓝牙接口
 * - 使用FreeRTOS双核处理，低占用
 * - 基于官方 M5ModuleLLM + M5Unified + M5GFX
 */

// 使用 M5Unified 推荐驱动
#include <M5Unified.h>
#include <M5ModuleLLM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

// ========== 全局配置存储 ==========
struct Config {
    char wifi_ssid[32];
    char wifi_password[64];
    bool  config_valid;
};

Config g_config;
Preferences prefs;

// ========== 应用定义 ==========
#define MAX_APPS 4

struct App {
    const char* name;
    const char* icon;
    uint16_t    color;
};

const App apps[MAX_APPS] = {
    {"聊天",    "💬", 0x45FD},   // 蓝色
    {"控制",    "🎮", 0x7D40},   // 绿色
    {"表情",    "😊", 0xFD20},   // 黄色
    {"设置",    "⚙️", 0x7BEF}    // 紫色
};

// ========== 全局对象 ==========
M5ModuleLLM     module_llm;
WebServer       server(80);
WebSocketsServer webSocket(81);
HardwareSerial  deviceSerial(1);  // 外部设备串口

// ========== APP状态 ==========
int   currentAppPage = 0;         // 当前页 (0: 桌面, 1+: 应用)
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

// ========== 传感器状态 ==========
float accX, accY, accZ;
float gyroX, gyroY, gyroZ;
bool  imu_ok = false;
bool  llm_connected = false;
uint32_t lastShakeTime = 0;
const uint32_t SHAKE_COOLDOWN = 1000;
int currentEmoji = 0;
int lastEmoji = -1;

// ========== 设备控制 (预留蓝牙接口) ==========
struct Device {
    String name;
    String id;
    bool connected;
};
std::vector<Device> devices;
bool bluetooth_enabled = false;  // 预留蓝牙标志

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

// ========== 颜色定义 ==========
const uint16_t BG_COLOR     = M5.Display.color565(250, 250, 250);  // 背景白色
const uint16_t TEXT_COLOR   = M5.Display.color565(30, 30, 30);        // 文字深色
const uint16_t CARD_BORDER  = M5.Display.color565(220, 220, 220);    // 卡片边框

// ========== 回调函数 ==========
void onLlmResult(String data, bool isFinish, int index);

// ========== 函数声明 ==========
bool loadConfig();
void saveConfig();
void drawSplashAnimation();
void drawLoadingSpinner(int centerX, int centerY, int radius, int angle);
void initHardware();
bool initNetwork();
void initLlmModule();
void checkSensors();
void drawUI();
void drawSplashScreen();
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
void handleWebConfig();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void broadcastMessage(String type, String data);
void llmTask(void *arg);
void updateSensors();

// ========== 加载配置 ==========
bool loadConfig() {
    prefs.begin("m5_llm", false);
    if (!prefs.isKey("config_v")) {
        prefs.end();
        return false;
    }
    g_config.config_valid = prefs.getBool("valid", false);
    if (!g_config.config_valid) {
        prefs.end();
        return false;
    }
    prefs.getString("ssid", g_config.wifi_ssid, 32);
    prefs.getString("pass", g_config.wifi_password, 64);
    prefs.end();
    return true;
}

// ========== 保存配置 ==========
void saveConfig() {
    prefs.begin("m5_llm", false);
    prefs.putBool("valid", g_config.config_valid);
    prefs.putString("ssid", g_config.wifi_ssid);
    prefs.putString("pass", g_config.wifi_password);
    prefs.end();
}

// ========== 绘制加载旋转动画 ==========
void drawLoadingSpinner(int centerX, int centerY, int radius, int angle) {
    int thickness = 6;
    // 绘制背景圆环
    for (int i = 0; i < 360; i += 10) {
        int x1 = centerX + (radius - thickness) * cos(i * PI / 180);
        int y1 = centerY + (radius - thickness) * sin(i * PI / 180);
        int x2 = centerX + radius * cos(i * PI / 180);
        int y2 = centerY + radius * sin(i * PI / 180);
        M5.Display.drawLine(x1, y1, x2, y2, M5.Display.color565(200, 200, 200));
    }
    // 绘制进度弧
    for (int i = angle; i < angle + 270; i += 10) {
        int ia = i % 360;
        int x1 = centerX + (radius - thickness) * cos(ia * PI / 180);
        int y1 = centerY + (radius - thickness) * sin(ia * PI / 180);
        int x2 = centerX + radius * cos(ia * PI / 180);
        int y2 = centerY + radius * sin(ia * PI / 180);
        M5.Display.drawLine(x1, y1, x2, y2, apps[0].color);
    }
}

// ========== 启动动画 ==========
void drawSplashAnimation() {
    M5.Display.fillScreen(BG_COLOR);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("M5 LLM Robot", SCREEN_WIDTH/2, 60);
    M5.Display.setTextSize(1);
    M5.Display.drawString("初始化中...", SCREEN_WIDTH/2, 90);
    M5.Display.setTextDatum(TL_DATUM);

    // 旋转加载动画
    for (int angle = 0; angle < 360; angle += 15) {
        drawLoadingSpinner(SCREEN_WIDTH/2, 160, 30, angle);
        M5.Display.display();
        delay(30);
    }
}

// ========== 初始化硬件 ==========
void initHardware() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(BG_COLOR);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setTextWrap(false);

    // 开启扬声器
    M5.Speaker.begin();
}

// ========== 检查传感器 ==========
void checkSensors() {
    imu_ok = M5.Imu.available();
    if (imu_ok) {
        M5.Imu.getAccel(&accX, &accY, &accZ);
    }
}

// ========== 初始化网络 ==========
bool initNetwork() {
    if (!g_config.config_valid) {
        M5.Display.fillScreen(BG_COLOR);
        M5.Display.setTextSize(1.5);
        M5.Display.setCursor(15, 60);
        M5.Display.println("未找到WiFi配置");
        M5.Display.println("请通过浏览器配置");
        M5.Display.println("IP: 192.168.4.1");
        M5.Display.setCursor(15, 140);
        M5.Display.println("打开配置页面");
        M5.Display.println("输入WiFi信息");

        // 开启AP供配置
        WiFi.softAP("M5-LLM-Setup");
        delay(2000);
        return false;
    }

    M5.Display.fillScreen(BG_COLOR);
    M5.Display.setCursor(15, 60);
    M5.Display.printf("连接WiFi:\n%s\n", g_config.wifi_ssid);
    M5.Display.update();

    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        // 动画显示连接进度
        int centerX = SCREEN_WIDTH / 2;
        int angle = (attempts * 30) % 360;
        M5.Display.fillRect(centerX - 40, 130, 80, 80, BG_COLOR);
        drawLoadingSpinner(centerX, 170, 25, angle);
        M5.Display.display();
        delay(500);
        attempts++;
    }

    M5.Display.fillRect(15, 100, 280, 30, BG_COLOR);
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.setCursor(15, 100);
        M5.Display.printf("已连接! IP: %s", WiFi.localIP().toString().c_str());
        delay(1000);
        return true;
    } else {
        M5.Display.setCursor(15, 100);
        M5.Display.printf("连接失败");
        delay(2000);
        return false;
    }
}

// ========== 初始化LLM模块 ==========
void initLlmModule() {
    M5.Display.fillScreen(BG_COLOR);
    M5.Display.setCursor(15, 60);
    M5.Display.println("检测LLM模块...");
    M5.Display.update();

    // CoreS3 PortC: RX=18, TX=17
    module_llm.begin(&Serial2);
    Serial2.begin(115200, SERIAL_8N1, 18, 17);

    // 创建命令队列
    llmCommandQueue = xQueueCreate(10, sizeof(LlmCommand));

    // 创建LLM任务到核心0 (UI在核心1)
    xTaskCreatePinnedToCore(llmTask, "llmTask", 8192, NULL, 2, &llmTaskHandle, 0);

    // 检查连接
    llm_connected = module_llm.checkConnection();
    if (!llm_connected) {
        M5.Display.fillRect(15, 90, 280, 30, BG_COLOR);
        M5.Display.setCursor(15, 90);
        M5.Display.println("LLM模块未连接");
        delay(2000);
        return;
    }

    M5.Display.fillRect(15, 90, 280, 30, BG_COLOR);
    M5.Display.setCursor(15, 90);
    M5.Display.println("LLM模块已连接");
    M5.Display.println("设置波特率...");
    M5.Display.update();

    // 提速到1.5Mbps
    module_llm.setBaudRate(1500000);
    Serial2.end();
    Serial2.begin(1500000, SERIAL_8N1, 18, 17);
    module_llm.begin(&Serial2);

    // 复位模块
    module_llm.sys.reset();
    delay(500);

    M5.Display.fillRect(15, 120, 280, 60, BG_COLOR);
    M5.Display.setCursor(15, 120);
    M5.Display.println("初始化模型...");
    M5.Display.update();

    // 配置ASR - 中文
    m5_module_llm::ApiAsrSetupConfig_t asrConfig;
    asrConfig.model = "sherpa-ncnn-streaming-zipformer-zh-14M-2023-02-17";
    workIds.asr = module_llm.asr.setup(asrConfig, "asr_setup", "zh_CN");

    // 配置LLM - 中文提示词
    m5_module_llm::ApiLlmSetupConfig_t llmConfig;
    llmConfig.prompt = "你是一个友好的AI助手，回答简洁自然。";
    llmConfig.max_token_len = 256;
    workIds.llm = module_llm.llm.setup(llmConfig, "llm_setup");

    // 配置TTS - 中文
    m5_module_llm::ApiTtsSetupConfig_t ttsConfig;
    ttsConfig.model = "single_speaker_chinese_fast";
    workIds.tts = module_llm.tts.setup(ttsConfig, "tts_setup", "zh_CN");

    M5.Display.fillRect(15, 120, 280, 60, BG_COLOR);
    M5.Display.setCursor(15, 120);
    M5.Display.println("初始化完成!");
    M5.Display.update();
    delay(800);
}

// ========== 绘制启动屏幕 ==========
void drawSplashScreen() {
    M5.Display.fillScreen(BG_COLOR);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setCursor(15, 15);
    M5.Display.print("M5 LLM Robot");

    M5.Display.setTextSize(1.2);
    int y = 60;
    auto drawStatus = [&](const char* name, bool ok) {
        M5.Display.setCursor(15, y);
        M5.Display.print(name);
        M5.Display.setCursor(180, y);
        if (ok) {
            M5.Display.setTextColor(M5.Display.color565(50, 200, 50));
            M5.Display.print("✓ 正常");
        } else {
            M5.Display.setTextColor(M5.Display.color565(200, 50, 50));
            M5.Display.print("✗ 异常");
        }
        M5.Display.setTextColor(TEXT_COLOR);
        y += 30;
    };

    drawStatus("IMU传感器", imu_ok);
    drawStatus("WiFi连接", WiFi.status() == WL_CONNECTED);
    drawStatus("LLM模块", llm_connected);
    drawStatus("蓝牙接口", bluetooth_enabled ? "预留" : "预留");

    M5.Display.setTextSize(1);
    M5.Display.setCursor(15, 180);
    M5.Display.print("点击任意位置进入主界面");
}

// ========== 绘制桌面 ==========
void drawHomeLauncher() {
    // 背景白色
    M5.Display.fillScreen(BG_COLOR);

    // 标题
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setCursor(15, 15);
    M5.Display.print("M5 LLM Robot");

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
    int spacing_dots = 12;
    int start = centerX - spacing_dots / 2;
    M5.Display.drawCircle(start, y, dotR, CARD_BORDER);
    M5.Display.fillCircle(start, y, dotR + (currentAppPage == 0 ? 2 : 0), currentAppPage == 0 ? apps[0].color : CARD_BORDER);
    M5.Display.drawCircle(start + spacing_dots, y, dotR, CARD_BORDER);
    M5.Display.fillCircle(start + spacing_dots, y, dotR + (currentAppPage == 1 ? 2 : 0), currentAppPage == 1 ? apps[2].color : CARD_BORDER);
}

// ========== 绘制单个APP图标 - 带点击缩放动画效果 ==========
void drawAppIcon(int index, int offsetX, bool isPressed) {
    int cardW = 130;
    int cardH = 90;
    int cornerR = 12;

    int col = index % 2;
    int row = index / 2;
    int spacing = 20;
    int spacingY = 20;
    int startX = (SCREEN_WIDTH - (cardW * 2 + spacing)) / 2;
    int startY = 50;

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
    // 名称 - 中文
    M5.Display.setTextSize(1.5);
    M5.Display.setTextColor(WHITE);
    int textLen = strlen(apps[index].name);
    M5.Display.setCursor(drawX + (drawW - 10*textLen) / 2, drawY + 60);
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
        M5.Display.print(isResponding ? "思考中..." : "点击麦克风说话");
        lastResponding = isResponding;
    }

    // 麦克风按钮
    M5.Display.fillCircle(40, 210, 20, isResponding ? RED : GREEN);
    M5.Display.drawCircle(40, 210, 22, WHITE);

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
            String prefix = msg.role == "user" ? "你: " : "AI: ";
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

// ========== 绘制控制应用 - 预留蓝牙接口 ==========
void drawControlApp() {
    // 顶部栏
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, 45, apps[1].color);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1.8);
    M5.Display.setCursor(15, 12);
    M5.Display.print(apps[1].name);

    // 控制按钮 - WiFi重连
    M5.Display.fillRoundRect(10, 60, SCREEN_WIDTH - 20, 50, 8, apps[1].color);
    M5.Display.setTextSize(1.5);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(80, 82);
    M5.Display.print("重新连接WiFi");

    // 蓝牙扫描按钮 - 预留
    M5.Display.fillRoundRect(10, 120, SCREEN_WIDTH - 20, 50, 8, bluetooth_enabled ? apps[1].color : M5.Display.color565(200, 200, 200));
    M5.Display.setTextSize(1.5);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(80, 142);
    M5.Display.print("蓝牙扫描 (预留)");

    // 设备列表标题
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TEXT_COLOR);
    M5.Display.setCursor(15, 185);
    M5.Display.print("已连接设备:");

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
    M5.Display.print("摇晃切换表情!");

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

        drawSetting("WiFi", WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
        drawSetting("IP", WiFi.localIP().toString().c_str());
        drawSetting("LLM模块", llm_connected ? "已连接" : "未连接");
        drawSetting("IMU传感器", imu_ok ? "正常" : "异常");
        drawSetting("蓝牙接口", "已预留");
        drawSetting("版本", "v3.0.0");
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

// ========== 触摸处理 - 带点击动画 ==========
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
                    // 按压动画 - 重绘按压状态，产生缩放效果
                    drawHomeLauncher();
                    drawAppIcon(i, 0, true);
                    M5.Display.display();
                    delay(80);  // 保持一小段时间让用户看到动画
                    // 点击进入app
                    currentAppPage = i + 1;
                    fullRedrawNeeded = true;
                    M5.Speaker.tone(600, 60);  // 点击音效
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

// ========== LLM任务 - 运行在核心0，UI在核心1，分离降低UI占用 ==========
void llmTask(void *arg) {
    LlmCommand cmd;
    for(;;) {
        if (xQueueReceive(llmCommandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch(cmd.type) {
                case CMD_ASR_START:
                    isResponding = true;
                    chatNeedUpdate = true;
                    if (llm_connected) {
                        module_llm.tts.inference(workIds.tts, "请稍等，正在听");
                        // 等待ASR结果
                        String asrResult;
                        if (module_llm.asr.getResult(&asrResult)) {
                            processLlmInference(asrResult);
                        } else {
                            isResponding = false;
                        }
                    } else {
                        isResponding = false;
                    }
                    break;

                case CMD_CHAT_INFERENCE:
                    processLlmInference(cmd.data);
                    break;
            }
        }
        if (llm_connected) {
            module_llm.update();
        } else {
            delay(100);
        }
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

        if (llm_connected) {
            module_llm.tts.inference(workIds.tts, currentResponse);
        }
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

    if (llm_connected) {
        module_llm.llm.inference(workIds.llm, prompt, onLlmResult);
    }
}

// ========== 更新传感器 ==========
void updateSensors() {
    if (!imu_ok) return;

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
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>M5 LLM Robot 配置</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 600px; margin: 0 auto; padding: 20px; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; }
    input { width: 100%; padding: 8px; font-size: 16px; }
    button { background: #4CAF50; color: white; border: none; padding: 10px 20px; font-size: 16px; cursor: pointer; }
  </style>
</head>
<body>
  <h1>M5 LLM Robot WiFi配置</h1>
  <form method="post" action="/saveconfig">
    <div class="form-group">
      <label>WiFi SSID</label>
      <input type="text" name="ssid" required>
    </div>
    <div class="form-group">
      <label>WiFi 密码</label>
      <input type="password" name="password" required>
    </div>
    <button type="submit">保存配置</button>
  </form>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}

// ========== 保存配置 ==========
void handleWebConfig() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String ssid = server.arg("ssid");
        String pass = server.arg("password");
        ssid.toCharArray(g_config.wifi_ssid, 32);
        pass.toCharArray(g_config.wifi_password, 64);
        g_config.config_valid = true;
        saveConfig();

        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head>
<body>
  <h1>配置已保存!</h1>
  <p>设备将重启连接WiFi</p>
</body>
</html>
        )rawliteral";
        server.send(200, "text/html", html);
        delay(1000);
        ESP.restart();
    } else {
        handleWebRoot();
    }
}

// ========== WebSocket事件 ==========
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_DISCONNECTED) {
        // 断开
    } else if (type == WStype_CONNECTED) {
        // 连接
    } else if (type == WStype_TEXT) {
        // 处理消息
        broadcastMessage("log", "received");
    }
}

// ========== 广播消息 ==========
void broadcastMessage(String type, String data) {
    String msg = "{\"type\":\"" + type + "\",\"data\":" + data + "}";
    webSocket.broadcastTXT(msg);
}

// ========== setup ==========
void setup() {
    // 初始化硬件
    initHardware();

    // 播放启动动画
    drawSplashAnimation();

    // 加载保存的WiFi配置
    bool has_config = loadConfig();

    // 检查传感器
    checkSensors();

    // 初始化网络 - 如果有配置就连接，没有就开AP
    if (has_config) {
        initNetwork();
    }

    // 初始化LLM模块
    initLlmModule();

    // 启动Web服务
    if (WiFi.status() == WL_CONNECTED || !has_config) {
        if (!has_config) {
            server.on("/", handleWebRoot);
            server.on("/saveconfig", HTTP_POST, handleWebConfig);
        }
        server.begin();
        webSocket.begin();
        webSocket.onEvent(webSocketEvent);
    }

    // 显示启动结果屏幕
    drawSplashScreen();
    M5.Display.display();
}

// ========== loop ==========
void loop() {
    M5.update();
    handleTouch();
    updateSensors();
    drawUI();
    if (WiFi.status() == WL_CONNECTED) {
        server.handleClient();
        webSocket.loop();
    }
}