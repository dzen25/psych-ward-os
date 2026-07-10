#include <WiFi.h>
#include <esp_wifi.h>

const char* ssid = "";                  // ssid сети
const char* password = "";              // пароль сети


const int LED_TX = 2;                   // пин светодиода tx
const int LED_RX = 1;                   // пин светодиода rx
const unsigned long BLINK_MS = 30;

HardwareSerial UartBridge(1);
WiFiServer server(23);
WiFiClient client;

unsigned long txOffTime = 0;
unsigned long rxOffTime = 0;

void setup()
{
    pinMode(LED_TX, OUTPUT);
    pinMode(LED_RX, OUTPUT);
    digitalWrite(LED_TX, LOW);
    digitalWrite(LED_RX, LOW);

    Serial.begin(115200);
    delay(1500);                        // время открыть Serial Monitor до первых логов

    // Тестовое моргание при старте - 5 раз обоими светодиодами
    for (int i = 0; i < 5; i++) {
        digitalWrite(LED_TX, HIGH);
        digitalWrite(LED_RX, HIGH);
        delay(150);
        digitalWrite(LED_TX, LOW);
        digitalWrite(LED_RX, LOW);
        delay(150);
    }

    UartBridge.begin(115200, SERIAL_8N1, 20, 21);  // RX=20, TX=21 -> к RPi4

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.setTxPower(WIFI_POWER_11dBm);  // критично для стабильного коннекта на этой плате

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

        // Сигнал успешного подключения: горим 1 секунду, потом короткая вспышка
        digitalWrite(LED_TX, HIGH);
        digitalWrite(LED_RX, HIGH);
        delay(1000);
        digitalWrite(LED_TX, LOW);
        digitalWrite(LED_RX, LOW);
        delay(200);
        digitalWrite(LED_TX, HIGH);
        digitalWrite(LED_RX, HIGH);
        delay(80);
        digitalWrite(LED_TX, LOW);
        digitalWrite(LED_RX, LOW);

        server.begin();
        server.setNoDelay(true);
    } else {
        Serial.println("WiFi FAILED to connect.");
    }
}

void loop()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (!client || !client.connected())
        client = server.available();

    while (client.available()) {
        UartBridge.write(client.read());
        digitalWrite(LED_TX, HIGH);
        txOffTime = millis() + BLINK_MS;
    }
    while (UartBridge.available()) {
        client.write(UartBridge.read());
        digitalWrite(LED_RX, HIGH);
        rxOffTime = millis() + BLINK_MS;
    }
    if (txOffTime && millis() > txOffTime) { digitalWrite(LED_TX, LOW); txOffTime = 0; }
    if (rxOffTime && millis() > rxOffTime) { digitalWrite(LED_RX, LOW); rxOffTime = 0; }
}