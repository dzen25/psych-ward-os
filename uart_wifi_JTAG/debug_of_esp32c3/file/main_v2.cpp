#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>

const char* ssid = "OpenWrt2.4";
const char* password = "623636889";

const int POWER_CTRL_PIN = 1;           // затвор N-MOSFET -> питание RPi4 (GPIO19 не выведен на QT Py C3, занят native-USB D+/D-)
const unsigned long RESTART_OFF_MS = 3000;  // сколько держим питание выключенным при restart

HardwareSerial UartBridge(1);
WiFiServer server(23);
WiFiClient client;
WebServer httpServer(80);

// --- Динамическое переключение power-save режима WiFi ---
unsigned long lastActivityTime = 0;
bool psActive = false;               // true -  в режиме NONE (без сна, без задержки)
const unsigned long IDLE_TIMEOUT_MS = 10000;  // через сколько мс простоя уходим в энергосбережение

void setPowerSaveMode(bool wantActive) {
    if (wantActive == psActive) return;   // уже в нужном режиме, не дёргаем лишний раз
    if (wantActive) {
        esp_wifi_set_ps(WIFI_PS_NONE);       // полная мощность, без задержки первого пакета
    } else {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // энергосбережение, меньше нагрев в простое
    }
    psActive = wantActive;
}

// --- Управление питанием RPi4 через MOSFET ---
void powerOn() {
    digitalWrite(POWER_CTRL_PIN, HIGH);
    Serial.println("RPi4 power: ON");
}

void powerOff() {
    digitalWrite(POWER_CTRL_PIN, LOW);
    Serial.println("RPi4 power: OFF");
}

void handlePowerOn() {
    powerOn();
    httpServer.send(200, "text/plain", "OK: power on\n");
}

void handlePowerOff() {
    powerOff();
    httpServer.send(200, "text/plain", "OK: power off\n");
}

void handlePowerRestart() {
    httpServer.send(200, "text/plain", "OK: restarting\n");
    powerOff();
    delay(RESTART_OFF_MS);
    powerOn();
}

void setup()
{
    pinMode(POWER_CTRL_PIN, OUTPUT);
    digitalWrite(POWER_CTRL_PIN, HIGH);  // питание остаётся включённым при старте/перезагрузке ESP32

    Serial.begin(115200);
    delay(1500);                        // время открыть Serial Monitor до первых логов

    // Увеличенные буферы UART - важно для потокового вывода (например логов sel4test)
    UartBridge.setRxBufferSize(1024);
    UartBridge.setTxBufferSize(1024);
    UartBridge.begin(115200, SERIAL_8N1, 20, 21);  // RX=20, TX=21 -> к RPi4

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);        // стартуем в активном режиме - мгновенный отклик сразу после подключения
    psActive = true;
    WiFi.setTxPower(WIFI_POWER_11dBm);   // критично для стабильного коннекта на этой плате

    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Connected! IP address: ");
        Serial.println(WiFi.localIP());

        server.begin();
        server.setNoDelay(true);
        lastActivityTime = millis();   // чтобы не уйти в сон сразу же после подключения

        httpServer.on("/on", handlePowerOn);
        httpServer.on("/off", handlePowerOff);
        httpServer.on("/restart", handlePowerRestart);
        httpServer.begin();
    } else {
        Serial.println("WiFi FAILED to connect.");
    }
}

void loop()
{
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastReconnectAttempt = 0;
        if (millis() - lastReconnectAttempt > 2000) {
            Serial.println("WiFi lost, reconnecting...");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            lastReconnectAttempt = millis();
        }
        return;
    }

    httpServer.handleClient();

    if (!client || !client.connected()) {
        client = server.available();
        if (client) {
            client.setNoDelay(true);   // явно отключаем Nagle для этого соединения
        }
    }

    static uint8_t buf[256];
    bool hadActivity = false;

    // Из сети -> в UART, пачками, а не по байту
    while (client.available()) {
        int n = client.read(buf, min((int)client.available(), (int)sizeof(buf)));
        if (n > 0) {
            UartBridge.write(buf, n);
            hadActivity = true;
        }
    }

    // Из UART -> в сеть, пачками
    while (UartBridge.available()) {
        int n = UartBridge.read(buf, min((int)UartBridge.available(), (int)sizeof(buf)));
        if (n > 0) {
            client.write(buf, n);
            hadActivity = true;
        }
    }

    // Переключение power-save: активность -> полная мощность, простой 2с -> сон
    if (hadActivity) {
        setPowerSaveMode(true);
        lastActivityTime = millis();
    } else if (psActive && millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
        setPowerSaveMode(false);
    }
}
