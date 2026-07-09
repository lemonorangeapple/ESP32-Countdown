#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ==================== 预设固定 Wi-Fi（备用） ====================
const char* default_ssid = "June";
const char* default_password = "June20120720";

// ==================== NTP 配置 ====================
const char* ntpServer = "ntp.aliyun.com";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

// ==================== API 配置 ====================
const char* apiUrl = "https://cd.imjcj.eu.org/api";

// ==================== 配网服务配置 ====================
const char* ap_ssid = "June_ESP32_Countdown";
const char* ap_password = "";
WebServer server(80);
DNSServer dnsServer;

Preferences preferences;

// ==================== 倒计时事件 ====================
struct CountdownEvent {
  String name;
  time_t targetTimestamp;
};
CountdownEvent events[10];
int eventCount = 0;

// ==================== 状态机 ====================
enum SystemMode { MODE_NORMAL, MODE_PROVISIONING };
SystemMode systemMode = MODE_NORMAL;

enum WifiState { WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_RECONNECTING };
WifiState wifiState = WIFI_DISCONNECTED;
unsigned long wifiConnectStart = 0;
const unsigned long wifiConnectTimeout = 30000; // 单次连接尝试30秒
unsigned long disconnectStartTime = 0;
const unsigned long reconnectTotalTimeout = 30 * 60 * 1000UL; // 半小时

enum NtpState { NTP_NOT_SYNCED, NTP_SYNCING, NTP_SYNCED };
NtpState ntpState = NTP_NOT_SYNCED;
unsigned long ntpSyncStart = 0;
const unsigned long ntpSyncTimeout = 15000;

unsigned long lastAPICheck = 0;
const unsigned long apiCheckInterval = 3600000; // 1小时

unsigned long lastShowTime = 0;
const unsigned long showInterval = 1000;

bool ntpEverSynced = false;
bool wifiConnectingRequested = false;

// 配网相关变量
bool provisioningStarted = false;
String new_ssid = "";
String new_password = "";

// ==================== 函数声明 ====================
void startWiFiConnect();
void checkWiFiConnection();
void startNtpSync();
void checkNtpSync();
void fetchAndParseData();
void parseJSON(String jsonString);
time_t parseDateTimeToTimestamp(String datetimeStr);
long getRemainingSeconds(time_t targetTimestamp);
void formatRemainingTime(long totalSeconds, char* buffer, size_t bufSize);
void showCountdown();
void checkWiFiReconnect();
void startProvisioning();
void handleProvisioning();
void handleRoot();
void handleConnect();
void saveWiFiCredentials(String ssid, String pwd);
bool loadWiFiCredentials();
void switchToNormalMode();

// ==================== 存储操作 ====================
bool loadWiFiCredentials() {
  preferences.begin("wifi", false);
  String ssid = preferences.getString("ssid", "");
  String pwd = preferences.getString("password", "");
  preferences.end();
  if (ssid.length() > 0) {
    new_ssid = ssid;
    new_password = pwd;
    return true;
  }
  return false;
}

void saveWiFiCredentials(String ssid, String pwd) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pwd);
  preferences.end();
  new_ssid = ssid;
  new_password = pwd;
}

// ==================== 配网 Web 页面 ====================
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32 配网</title>
<style>body{font-family:Arial;text-align:center;margin-top:50px;} input,button{padding:10px;margin:5px;width:80%;max-width:300px;} button{background-color:#4CAF50;color:white;border:none;}</style>
</head>
<body>
<h2>Wi-Fi 设置</h2>
<form action="/connect" method="POST">
<input type="text" name="ssid" placeholder="Wi-Fi SSID" required><br>
<input type="password" name="password" placeholder="密码"><br>
<button type="submit">连接</button>
</form>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", FPSTR(htmlPage));
}

void handleConnect() {
  if (server.hasArg("ssid")) {
    String ssid = server.arg("ssid");
    String pwd = server.arg("password");
    server.send(200, "text/html", "<html><meta charset='UTF-8'><body><h2>配置已接收，设备将重启...</h2></body></html>");
    delay(1000);
    saveWiFiCredentials(ssid, pwd);
    ESP.restart(); // 重启后使用新凭证连接
  } else {
    server.send(400, "text/html", "<html><meta charset='UTF-8'><body><h2>缺少SSID</h2></body></html>");
  }
}

void startProvisioning() {
  Serial.println("启动配网模式...");
  systemMode = MODE_PROVISIONING;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP地址: ");
  Serial.println(apIP);
  server.on("/", handleRoot);
  server.on("/connect", HTTP_POST, handleConnect);
  server.begin();
  dnsServer.start(53, "*", apIP);
  provisioningStarted = true;
}

void handleProvisioning() {
  if (!provisioningStarted) return;
  dnsServer.processNextRequest();
  server.handleClient();
}

// ==================== 正常模式 WiFi 连接（非阻塞） ====================
void startWiFiConnect() {
  if (wifiConnectingRequested) return;
  wifiConnectingRequested = true;
  Serial.println("开始连接 Wi-Fi...");
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(new_ssid.c_str(), new_password.c_str());
  wifiState = WIFI_CONNECTING;
  wifiConnectStart = millis();
}

void checkWiFiConnection() {
  if (wifiState != WIFI_CONNECTING) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiState = WIFI_CONNECTED;
    wifiConnectingRequested = false;
    disconnectStartTime = 0;
    Serial.println("\nWi-Fi 连接成功!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    if (!ntpEverSynced) {
      startNtpSync();
    } else {
      fetchAndParseData();
      lastAPICheck = millis();
    }
  } else if (millis() - wifiConnectStart > wifiConnectTimeout) {
    Serial.println("\nWi-Fi 连接超时，将重试或进入配网模式...");
    wifiState = WIFI_DISCONNECTED;
    wifiConnectingRequested = false;
    // 如果连接失败，进入配网模式
    if (systemMode == MODE_NORMAL) {
      startProvisioning();
    }
  } else {
    static unsigned long lastDot = 0;
    if (millis() - lastDot > 2000) {
      Serial.print(".");
      lastDot = millis();
    }
  }
}

// ==================== 断线重连（半小时超时） ====================
void checkWiFiReconnect() {
  if (systemMode != MODE_NORMAL) return;
  if (wifiState == WIFI_CONNECTED && WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi 断开，开始重连...");
    wifiState = WIFI_RECONNECTING;
    disconnectStartTime = millis();
    return;
  }
  if (wifiState == WIFI_RECONNECTING) {
    unsigned long now = millis();
    if (now - disconnectStartTime >= reconnectTotalTimeout) {
      Serial.println("\n重连超时（30分钟），进入配网模式...");
      startProvisioning();
      return;
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiState = WIFI_CONNECTED;
      Serial.println("\nWi-Fi 重连成功！");
      fetchAndParseData();
      lastAPICheck = millis();
      return;
    }
    // 每5秒尝试一次重连
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt >= 5000) {
      lastReconnectAttempt = now;
      Serial.print("R");
      WiFi.reconnect();
    }
  }
}

// ==================== NTP 同步（非阻塞） ====================
void startNtpSync() {
  if (ntpState == NTP_NOT_SYNCED) {
    Serial.println("同步 NTP 时间...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    ntpState = NTP_SYNCING;
    ntpSyncStart = millis();
  }
}

void checkNtpSync() {
  if (ntpState != NTP_SYNCING) return;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    ntpState = NTP_SYNCED;
    ntpEverSynced = true;
    Serial.println("\n时间同步成功");
    fetchAndParseData();
    lastAPICheck = millis();
  } else if (millis() - ntpSyncStart > ntpSyncTimeout) {
    ntpState = NTP_NOT_SYNCED;
    Serial.println("\n时间同步超时，稍后重试");
  } else {
    static unsigned long lastNtpDot = 0;
    if (millis() - lastNtpDot > 1000) {
      Serial.print(".");
      lastNtpDot = millis();
    }
  }
}

// ==================== API 获取与解析 ====================
void fetchAndParseData() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpEverSynced && ntpState != NTP_SYNCED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient http;
  http.begin(client, apiUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("API 获取成功，解析 JSON...");
    parseJSON(payload);
  } else {
    Serial.printf("API 请求失败, HTTP %d\n", httpCode);
  }
  http.end();
}

time_t parseDateTimeToTimestamp(String datetimeStr) {
  struct tm tm;
  int year, month, day, hour, minute;
  if (sscanf(datetimeStr.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) == 5) {
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    return mktime(&tm);
  }
  return 0;
}

void parseJSON(String jsonString) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonString);
  if (error) {
    Serial.print("JSON 解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  eventCount = 0;
  JsonArray array = doc.as<JsonArray>();
  for (JsonObject obj : array) {
    if (eventCount >= 10) break;
    const char* name = obj["name"];
    const char* datetime = obj["datetime"];
    if (name && datetime) {
      events[eventCount].name = String(name);
      events[eventCount].targetTimestamp = parseDateTimeToTimestamp(String(datetime));
      if (events[eventCount].targetTimestamp == 0) {
        Serial.printf("警告：时间解析失败 -> %s\n", datetime);
        continue;
      }
      eventCount++;
    }
  }
  Serial.printf("成功解析 %d 条倒计时事件\n", eventCount);
  showCountdown();
}

long getRemainingSeconds(time_t targetTimestamp) {
  time_t now;
  time(&now);
  return (long)(targetTimestamp - now);
}

void formatRemainingTime(long totalSeconds, char* buffer, size_t bufSize) {
  if (totalSeconds < 0) {
    snprintf(buffer, bufSize, "已过期");
    return;
  }
  long days = totalSeconds / 86400;
  long hours = (totalSeconds % 86400) / 3600;
  long minutes = (totalSeconds % 3600) / 60;
  long seconds = totalSeconds % 60;
  snprintf(buffer, bufSize, "%ld天 %02ld时 %02ld分 %02ld秒", days, hours, minutes, seconds);
}

void showCountdown() {
  if (eventCount == 0) {
    Serial.println("暂无倒计时数据");
    return;
  }
  struct tm now;
  if (!getLocalTime(&now)) {
    Serial.println("无法获取当前时间");
    return;
  }
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &now);
  Serial.println("==========================================");
  Serial.printf("当前时间: %s\n", timeStr);
  Serial.println("倒计时（精确到秒）:");
  for (int i = 0; i < eventCount; i++) {
    long remaining = getRemainingSeconds(events[i].targetTimestamp);
    char remainingStr[50];
    formatRemainingTime(remaining, remainingStr, sizeof(remainingStr));
    Serial.printf("  %s : %s\n", events[i].name.c_str(), remainingStr);
  }
  Serial.println("==========================================");
}

// ==================== 切换到正常模式（已配网） ====================
void switchToNormalMode() {
  systemMode = MODE_NORMAL;
  provisioningStarted = false;
  wifiState = WIFI_DISCONNECTED;
  ntpState = NTP_NOT_SYNCED;
  ntpEverSynced = false;
  eventCount = 0;
  startWiFiConnect();
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32 倒计时系统（集成配网版）");

  if (loadWiFiCredentials()) {
    Serial.println("已读取保存的 Wi-Fi 凭证，尝试连接...");
    startWiFiConnect();
  } else {
    Serial.println("未找到有效的 Wi-Fi 凭证，进入配网模式。");
    startProvisioning();
  }
}

// ==================== loop ====================
void loop() {
  if (systemMode == MODE_PROVISIONING) {
    handleProvisioning();
    delay(10);
    return;
  }

  // 正常模式状态机
  if (wifiState == WIFI_DISCONNECTED) {
    startWiFiConnect();
  } else if (wifiState == WIFI_CONNECTING) {
    checkWiFiConnection();
  } else if (wifiState == WIFI_RECONNECTING) {
    checkWiFiReconnect();
  }

  if (wifiState == WIFI_CONNECTED) {
    if (ntpState == NTP_NOT_SYNCED && !ntpEverSynced) {
      startNtpSync();
    } else if (ntpState == NTP_SYNCING) {
      checkNtpSync();
    }
  }

  // 倒计时显示（需要时间同步过且有数据）
  if (ntpEverSynced && eventCount > 0) {
    unsigned long now = millis();
    if (now - lastShowTime >= showInterval) {
      showCountdown();
      lastShowTime = now;
    }
  }

  // 定时刷新 API（需要 Wi-Fi 在线）
  if (wifiState == WIFI_CONNECTED && ntpEverSynced) {
    unsigned long now = millis();
    if (now - lastAPICheck >= apiCheckInterval) {
      fetchAndParseData();
      lastAPICheck = now;
    }
  }

  // 断线重连处理（仅当处于正常模式且当前不是重连状态时）
  if (systemMode == MODE_NORMAL && wifiState == WIFI_CONNECTED) {
    checkWiFiReconnect();
  }

  delay(10);
}