#include <Arduino.h>
#include <WiFi.h>
#include "wifi_credentials.h"

#define LED_PIN 4

void setup() {
  pinMode(LED_PIN, OUTPUT);

  Serial.begin(115200);
  Serial.println("ESP32 Blink Test Started");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
}

void loop() {
  static bool wasConnected = false;
  const bool isConnected = WiFi.status() == WL_CONNECTED;

  if (isConnected && !wasConnected) {
    Serial.print("\nConnected to Wi-Fi. IP address: ");
    Serial.println(WiFi.localIP());
  } else if (!isConnected && wasConnected) {
    Serial.println("\nWi-Fi disconnected.");
  }
  wasConnected = isConnected;

  digitalWrite(LED_PIN, HIGH);
  delay(1000);

  digitalWrite(LED_PIN, LOW);
  delay(1000);
}
