#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <WiFiClient.h>
#include <ESP8266HTTPUpdateServer.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// Назначение пинов по вашей схеме
const int pinRelay = 5;  // D1 (Реле)
const int pinBtn   = 0;  // D3 (Кнопка настройки при старте)
const int pinSCK   = 14; // D5 (Clock датчика)
const int pinDT    = 12; // D6 (Data датчика)

// Структура для сохранения в EEPROM с полями безопасности
struct Settings {
  uint32_t configID;   
  char ssid[32];       
  char pass[32];       
  long lowLevel;       
  long highLevel;      
  unsigned long timeoutValue; 
  char otaUser[32];    // Динамический логин для страницы /update
  char otaPass[32];    // Динамический пароль для страницы /update
} user_data;

bool pumpState = false;
long currentPressure = 0;
int currentPercent = 0;
bool isConfigMode = false;
bool needRestart = false; 

// ПЕРЕМЕННЫЕ ЛОГИКИ, ТАЙМАУТА И СИГНАЛА
bool manualMode = false;       
unsigned long pumpStartTime = 0; 
bool timeoutTriggered = false;  
unsigned long runningSeconds = 0; 
long secondsLeft = 0;             
int wifiSignalPct = 0;            
long wifiSignalDbm = 0;           

// Функция чтения датчика TM7711 с отключением прерываний
long readTM7711() {
  unsigned long count = 0;
  noInterrupts(); 
  for (int i = 0; i < 24; i++) {
    digitalWrite(pinSCK, HIGH);
    delayMicroseconds(5);
    count = count << 1;
    digitalWrite(pinSCK, LOW);
    delayMicroseconds(5);
    if (digitalRead(pinDT)) count++;
  }
  digitalWrite(pinSCK, HIGH); 
  delayMicroseconds(5);
  digitalWrite(pinSCK, LOW);
  delayMicroseconds(5);
  interrupts(); 
  
  long signed_data = count;
  if (count & 0x800000) signed_data |= 0xFF000000;
  return signed_data;
}

// ОРИГИНАЛЬНЫЙ СТАБИЛЬНЫЙ ФИЛЬТР 100 МС ДЛЯ ЧИПА TM7711 (ВОЗВРАЩЕН НА МЕСТО)
long getSmoothPressure() {
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += readTM7711();
    delay(100); 
  }
  return sum / 5;
}

int calculatePercent(long current, long low, long high) {
  if (high <= low) return 0;
  float pct = ((float)(current - low) / (float)(high - low)) * 100.0;
  int result = (int)pct;
  if (result < 0) result = 0;
  if (result > 100) result = 100;
  return result;
}

int rssiToPercentage(long rssi) {
  if (rssi >= -50) return 100;
  if (rssi <= -100) return 0;
  return 2 * (rssi + 100);
}

String getWifiQualityText(int pct) {
  if (pct > 80) return "<span style='color:green; font-weight:bold;'>Отличный</span>";
  if (pct > 55) return "<span style='color:#28a745; font-weight:bold;'>Хороший</span>";
  if (pct > 30) return "<span style='color:orange; font-weight:bold;'>Слабый</span>";
  return "<span style='color:red; font-weight:bold;'>Плохой (Помехи!)</span>";
}

void startConfigMode() {
  isConfigMode = true;
  WiFi.softAPdisconnect(true); 
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Smart_pump");
  Serial.println("\nРежим конфигурации активирован!");
}

void startClientMode() {
  isConfigMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(user_data.ssid, user_data.pass);
  Serial.print("\nПодключение к WiFi: "); Serial.println(user_data.ssid);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi подключен успешно!");
  } else {
    Serial.println("\nРаботаем автономно.");
  }

  delay(100); 
  currentPressure = getSmoothPressure();
  currentPercent = calculatePercent(currentPressure, user_data.lowLevel, user_data.highLevel);
  
  if (currentPercent > 50) {
    pumpState = true; pumpStartTime = millis();
    digitalWrite(pinRelay, HIGH); 
  } else {
    pumpState = false; digitalWrite(pinRelay, LOW);
  }
}

// ОБРАБОТЧИКИ СЕРВЕРА
void setupWebServerHandlers() {
  
  // 1. ГЛАВНАЯ СТРАНИЦА ВЕБ-ИНТЕРФЕЙСА
  server.on("/", []() {
    currentPressure = getSmoothPressure();
    
    if (isConfigMode) {
      String safeSSID = String(user_data.ssid);
      String safePass = String(user_data.pass);
      if (user_data.configID != 0xABCD1234) { safeSSID = ""; safePass = ""; }

      String html = "<html><head><meta charset='UTF-8'></head>";
      html += "<body style='font-family:Arial; padding:20px; max-width:400px; margin:auto;'>";
      html += "<h2>Настройки Smart_pump (Режим AP)</h2>";
      html += "<div style='background:#eee; padding:10px; margin-bottom:15px; border-radius:5px;'>Текущее давление датчика: <b>" + String(currentPressure) + "</b></div>";
      html += "<form action='/save' method='POST'>";
      html += "<h3>1. Параметры Wi-Fi</h3>";
      html += "WiFi SSID:<br><input type='text' name='ssid' value='" + safeSSID + "' style='width:100%; padding:8px; margin-bottom:15px;'>";
      html += "WiFi Pass:<br><input type='password' name='pass' value='" + safePass + "' style='width:100%; padding:8px; margin-bottom:15px;'>";
      
      // ПОЛЯ БЕЗОПАСНОСТИ ОТОБРАЖАЮТСЯ СТРОГО В РЕЖИМЕ КОНФИГУРАТОРА (AP)
      html += "<h3>2. Безопасность обновления (/update)</h3>";
      html += "Логин администратора:<br><input type='text' name='ota_u' value='" + String(user_data.otaUser) + "' style='width:100%; padding:8px; margin-bottom:10px;'>";
      html += "Пароль администратора:<br><input type='password' name='ota_p' value='" + String(user_data.otaPass) + "' style='width:100%; padding:8px; margin-bottom:15px;'>";
      
      html += "<h3>3. Уровни давления и таймаут</h3>";
      html += "Нижний порог:<br><input type='text' name='low' value='" + String(user_data.lowLevel) + "' style='width:100%; padding:8px; margin-bottom:15px;'>";
      html += "Верхний порог:<br><input type='text' name='high' value='" + String(user_data.highLevel) + "' style='width:100%; padding:8px; margin-bottom:15px;'>";
      html += "Таймаут (сек):<br><input type='text' name='timeout' value='" + String(user_data.timeoutValue) + "' style='width:100%; padding:8px; margin-bottom:20px;'>";
      html += "<input type='submit' value='Сохранить все настройки' style='background:green; color:white; padding:12px; width:100%; border-radius:4px; cursor:pointer; font-size:1.1em; border:0;'>";
      html += "</form></body></html>";
      server.send(200, "text/html", html);
    } 
    else {
      // РАБОЧИЙ РЕЖИМ (ПОЛЯ ЛОГИНА/ПАРОЛЯ OTA ОТСЮДА ПОЛНОСТЬЮ УДАЛЕНЫ)
      currentPercent = calculatePercent(currentPressure, user_data.lowLevel, user_data.highLevel);
      String stateText = "";
      if (timeoutTriggered) {
        stateText = "<b style='color:purple; font-size:1.4em;'>АВАРИЯ: ТАЙМАУТ ПРЕВЫШЕН!</b>";
      } else if (manualMode) {
        stateText = pumpState ? "<b style='color:orange; font-size:1.4em;'>РУЧНОЙ РЕЖИМ: ВКЛ</b>" : "<b style='color:orange; font-size:1.4em;'>РУЧНОЙ РЕЖИМ: ВЫКЛ</b>";
      } else {
        stateText = pumpState ? "<b style='color:green; font-size:1.4em;'>АВТО: ОТКАЧКА</b>" : "<b style='color:red; font-size:1.4em;'>АВТО: ОЖИДАНИЕ</b>";
      }
      
      String html = "<html><head><meta charset='UTF-8'></head><body style='font-family:Arial; text-align:center; padding-top:20px; max-width:500px; margin:auto;'>";
      html += "<h2>Дренажный колодец</h2><br>";
      html += "<div style='width:80%; background:#ddd; margin:auto; border-radius:15px; overflow:hidden; border:1px solid #999;'>";
      html += "<div id='pbar' style='width:" + String(currentPercent) + "%; background:#3498db; height:30px; line-height:30px; color:white; font-weight:bold; transition: width 0.5s;'>" + String(currentPercent) + "%</div></div><br>";
      
      html += "<p>Статус системы: <span id='status_text'>" + stateText + "</span></p>";
      
      String timersDisplay = pumpState ? "block" : "none";
      html += "<div id='timers_block' style='background:#e2f0d9; padding:10px; margin:10px auto; width:80%; border-radius:5px; border:1px solid #ccd7c5; display:" + timersDisplay + ";'>";
      html += "⏱ Время работы: <b id='t_run'>" + String(runningSeconds) + " сек</b><br>";
      html += "<span id='t_left_row'>⏳ До отключения: <b id='t_left'>" + String(secondsLeft) + " сек</b></span>";
      html += "</div>";
      
      html += "<div style='background:#f8f9fa; padding:15px; border-radius:8px; border:1px solid #e9ecef; margin:20px 0;'>";
      if (!manualMode) {
        html += "<a href='/set_manual' style='padding:10px 20px; background:#6c757d; color:white; text-decoration:none; border-radius:4px; font-weight:bold;'>Перейти в РУЧНОЙ режим</a>";
      } else {
        html += "<a href='/set_auto' style='padding:10px 20px; background:#007bff; color:white; text-decoration:none; border-radius:4px; font-weight:bold;'>Вернуть АВТО режим</a>";
        html += "<div style='margin-top:25px;'>"; 
        if (!pumpState) {
          html += "<a href='/pump_on' style='display:inline-block; padding:14px 30px; background:#28a745; color:white; text-decoration:none; border-radius:4px; font-weight:bold; width:80%; box-shadow: 0 2px 4px rgba(0,0,0,0.1);'>ВКЛЮЧИТЬ НАСОС</a>";
        } else {
          html += "<a href='/pump_off' style='display:inline-block; padding:14px 30px; background:#dc3545; color:white; text-decoration:none; border-radius:4px; font-weight:bold; width:80%; box-shadow: 0 2px 4px rgba(0,0,0,0.1);'>ВЫКЛЮЧИТЬ НАСОС</a>";
        }
        html += "</div>";
      }
      html += "</div>";

      html += "<div style='background:#eee; padding:20px; border-radius:8px; text-align:left; margin-top:20px;'>";
      html += "<h3 style='text-align:center; margin-top:0;'>Конфигурация устройства</h3>";
      html += "<form action='/save' method='POST'>";
      html += "<details><summary style='cursor:pointer; color:#007bff; font-weight:bold; margin-bottom:10px;'>Настройки Wi-Fi</summary>";
      html += "SSID:<br><input type='text' name='ssid' value='" + String(user_data.ssid) + "' style='width:100%; padding:6px; margin:5px 0 10px;'><br>";
      html += "Пароль:<br><input type='password' name='pass' value='" + String(user_data.pass) + "' style='width:100%; padding:6px; margin:5px 0 10px;'><br></details>";
      html += "Нижний порог:<br><input type='text' name='low' value='" + String(user_data.lowLevel) + "' style='width:100%; padding:6px; margin:5px 0 10px;'><br>";
      html += "Верхний порог:<br><input type='text' name='high' value='" + String(user_data.highLevel) + "' style='width:100%; padding:6px; margin:5px 0 10px;'><br>";
      html += "Таймаут (сек):<br><input type='text' name='timeout' value='" + String(user_data.timeoutValue) + "' style='width:100%; padding:6px; margin:5px 0 15px;'><br>";
      html += "<input type='submit' value='Применить изменения' style='background:#28a745; color:white; padding:10px; width:100%; border:0; border-radius:4px; font-weight:bold; cursor:pointer;'>";
      html += "</form></div>";

      html += "<p style='color:#555; margin-top:20px;'><small>📶 Wi-Fi: " + getWifiQualityText(wifiSignalPct) + " (<span id='w_pct'>" + String(wifiSignalPct) + "</span>% / <span id='w_dbm'>" + String(wifiSignalDbm) + "</span> dBm)</small></p>";
      html += "<p style='color:#999;'><small>Давление датчика: <span id='press_val'>" + String(currentPressure) + "</span></small></p>";
      
      html += "<script>";
      html += "setInterval(function() {";
      html += "  fetch('/api/status').then(response => response.json()).then(data => {";
      html += "    document.getElementById('press_val').innerText = data.pressure;";
      html += "    document.getElementById('w_pct').innerText = data.wifi_pct;";
      html += "    document.getElementById('w_dbm').innerText = data.wifi_rssi;";
      html += "    var pbar = document.getElementById('pbar'); pbar.style.width = data.percent + '%'; pbar.innerText = data.percent + '%';";
      html += "    if (data.pump_state) {";
      html += "      document.getElementById('timers_block').style.display = 'block';";
      html += "      document.getElementById('t_run').innerText = data.running_seconds + ' сек';";
      html += "      if (data.seconds_left == -1) { document.getElementById('t_left_row').style.display = 'none'; }";
      html += "      else { document.getElementById('t_left_row').style.display = 'inline'; document.getElementById('t_left').innerText = data.seconds_left + ' сек'; }";
      html += "    } else { document.getElementById('timers_block').style.display = 'none'; }";
      html += "  });";
      html += "}, 2000);"; 
      html += "</script>";

      html += "</body></html>";
      server.send(200, "text/html", html);
    }
  });

  // 2. ОБРАБОТЧИК ДЛЯ HOME ASSISTANT
  server.on("/api/status", []() {
    String json = "{";
    json += "\"pressure\":" + String(currentPressure) + ",";
    json += "\"percent\":" + String(currentPercent) + ",";
    json += "\"pump_state\":" + String(pumpState ? "true" : "false") + ",";
    json += "\"manual_mode\":" + String(manualMode ? "true" : "false") + ",";
    json += "\"timeout_triggered\":" + String(timeoutTriggered ? "true" : "false") + ",";
    json += "\"low_level\":" + String(user_data.lowLevel) + ",";
    json += "\"high_level\":" + String(user_data.highLevel) + ",";
    json += "\"timeout_value\":" + String(user_data.timeoutValue) + ",";
    json += "\"running_seconds\":" + String(runningSeconds) + ",";
    json += "\"seconds_left\":" + String(secondsLeft) + ",";
    json += "\"wifi_rssi\":" + String(wifiSignalDbm) + ",";   
    json += "\"wifi_pct\":" + String(wifiSignalPct) + "";    
    json += "}";
    server.send(200, "application/json", json);
  });

  // 3. ОБРАБОТЧИК СОХРАНЕНИЯ НАСТРОЕК
  server.on("/save", []() {
    String req_ssid = server.arg("ssid"); 
    String req_pass = server.arg("pass");
    long new_low    = server.arg("low").toInt(); 
    long new_high   = server.arg("high").toInt();
    unsigned long new_timeout = server.arg("timeout").toInt();

    bool wifiChanged = false;
    bool securityChanged = false;
    
    if (req_ssid.length() > 0 && req_ssid != "None" && req_ssid != "") {
      if (req_ssid != String(user_data.ssid)) {
        req_ssid.toCharArray(user_data.ssid, 32);
        wifiChanged = true;
      }
    }
    if (req_pass.length() > 0 && req_pass != "None" && req_pass != "") {
      if (req_pass != String(user_data.pass)) {
        req_pass.toCharArray(user_data.pass, 32);
        wifiChanged = true;
      }
    }

    // Сохранение OTA-паролей ТОЛЬКО если запрос отправлен из режима конфигурации
    if (isConfigMode) {
      String req_ota_u = server.arg("ota_u");
      String req_ota_p = server.arg("ota_p");
      
      if (req_ota_u.length() > 0 && req_ota_u != "None" && req_ota_u != "") {
        if (req_ota_u != String(user_data.otaUser)) {
          req_ota_u.toCharArray(user_data.otaUser, 32);
          securityChanged = true;
        }
      }
      if (req_ota_p.length() > 0 && req_ota_p != "None" && req_ota_p != "") {
        if (req_ota_p != String(user_data.otaPass)) {
          req_ota_p.toCharArray(user_data.otaPass, 32);
          securityChanged = true;
        }
      }
    }

    user_data.lowLevel = new_low; 
    user_data.highLevel = new_high; 
    user_data.timeoutValue = new_timeout; 
    user_data.configID = 0xABCD1234;

    EEPROM.put(0, user_data); 
    EEPROM.commit();

    if (isConfigMode || wifiChanged || securityChanged) {
      server.send(200, "text/html", "<html><meta charset='UTF-8'><body><h3>Данные сохранены. Перезагрузка системы...</h3></body></html>");
      needRestart = true; 
    } else {
      timeoutTriggered = false; 
      String html = "<html><meta charset='UTF-8'><body><h3>Настройки применены «на лету»!</h3>";
      html += "<script>setTimeout(function(){ window.location.href = '/'; }, 1500);</script></body></html>";
      server.send(200, "text/html", html);
    }
  });

  // 4. СЕРВИСНЫЕ И УПРАВЛЯЮЩИЕ ЭНДПОИНТЫ
  server.on("/pump_on", []() {
    if (isConfigMode || manualMode) { pumpState = true; pumpStartTime = millis(); timeoutTriggered = false; }
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/pump_off", []() {
    if (isConfigMode || manualMode) pumpState = false;
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/set_manual", []() {
    if (!isConfigMode) { manualMode = true; pumpState = false; digitalWrite(pinRelay, LOW); }
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/set_auto", []() {
    if (!isConfigMode) { manualMode = false; timeoutTriggered = false; }
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/go_config", []() {
    if (!isConfigMode) {
      server.send(200, "text/html", "<html><meta charset='UTF-8'><body><h3>Переключение в режим AP...</h3></body></html>");
      delay(500); startConfigMode();
    } else {
      server.sendHeader("Location", "/"); server.send(303);
    }
  });

  // 5. ИСПРАВЛЕННЫЙ ПЕРЕХВАТ СТРАНИЦЫ ОБНОВЛЕНИЯ (Блокирует пустые поля в памяти)
  server.on("/update", HTTP_GET, []() {
    // Если логин или пароль пустые (не заданы), доступ по воздуху ПОЛНОСТЬЮ блокируется!
    if (strlen(user_data.otaUser) == 0 || strlen(user_data.otaPass) == 0) {
      server.send(403, "text/html", "<html><meta charset='UTF-8'><body><h2>Ошибка 403: Доступ заблокирован. Установите логин и пароль в режиме конфигурации устройства!</h2></body></html>");
      return;
    }
    if (!server.authenticate(user_data.otaUser, user_data.otaPass)) {
      return server.requestAuthentication();
    }
    server.send(200, "text/html", "<html><head><meta charset='UTF-8'></head><body><h2>Обновление прошивки Smart_pump</h2><form method='POST' action='/update' enctype='multipart/form-data'>Выбор бинарного файла (.bin):<br><br><input type='file' name='firmware'><br><br><input type='submit' value='Запустить update' style='padding:8px 20px; background:#007bff; color:white; border:0; border-radius:4px; cursor:pointer;'></form></body></html>");
  });

  httpUpdater.setup(&server, "/update", user_data.otaUser, user_data.otaPass);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== СИСТЕМА УПРАВЛЕНИЯ НАСОСОМ ЗАПУЩЕНА ===");

  EEPROM.begin(512);
  pinMode(pinRelay, OUTPUT); pinMode(pinBtn, INPUT_PULLUP);
  pinMode(pinSCK, OUTPUT); pinMode(pinDT, INPUT);
  digitalWrite(pinRelay, LOW); digitalWrite(pinSCK, LOW);

  EEPROM.get(0, user_data);
  
  // При первом запуске нового чипа забиваем структуру нулями (Wi-Fi и OTA будут пустыми)
  if (user_data.configID != 0xABCD1234) { 
    memset(user_data.ssid, 0, 32);
    memset(user_data.pass, 0, 32);
    memset(user_data.otaUser, 0, 32); // СТРОГО ПУСТО ПРИ СТАРТЕ!
    memset(user_data.otaPass, 0, 32); // СТРОГО ПУСТО ПРИ СТАРТЕ!
    user_data.lowLevel = 2000;
    user_data.highLevel = 8000;
    user_data.timeoutValue = 60;
    user_data.configID = 0xABCD1234;
    EEPROM.put(0, user_data);
    EEPROM.commit();
  }

  setupWebServerHandlers();

  Serial.print("Ожидание 5 секунд для входа в настройки");
  unsigned long startWait = millis(); bool btnPressed = false; int lastDotSeconds = 0;
  while (millis() - startWait < 5000) {
    if (digitalRead(pinBtn) == LOW) { btnPressed = true; break; }
    int currentSeconds = (millis() - startWait) / 1000;
    if (currentSeconds != lastDotSeconds) { Serial.print("."); lastDotSeconds = currentSeconds; }
    delay(20); yield();
  }
  Serial.println();

  if (btnPressed || user_data.configID != 0xABCD1234) { startConfigMode(); } 
  else { startClientMode(); }

  server.begin();
}

void loop() {
  server.handleClient();

  if (needRestart) { delay(500); ESP.restart(); }

  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 500) {
    lastUpdate = millis();
    
    currentPressure = getSmoothPressure();
    
    if (!isConfigMode) {
      currentPercent = calculatePercent(currentPressure, user_data.lowLevel, user_data.highLevel);
      
      wifiSignalDbm = WiFi.RSSI();
      wifiSignalPct = rssiToPercentage(wifiSignalDbm);

      // Логика авто-режима
      if (!manualMode && !timeoutTriggered) {
        if (currentPressure >= user_data.highLevel && !pumpState) {
          pumpState = true; pumpStartTime = millis(); 
          Serial.println("Порог превышен! Авто-включение.");
        }
        if (currentPressure <= user_data.lowLevel && pumpState) {
          pumpState = false;
          Serial.println("Колодец пуст. Авто-выключение.");
        }
      }

      // Проверка защитного таймаута
      if (pumpState) {
        unsigned long workingTimeMs = millis() - pumpStartTime;
        runningSeconds = workingTimeMs / 1000;
        
        if (user_data.timeoutValue > 0) {
          long calculatedLeft = (long)user_data.timeoutValue - (long)runningSeconds;
          secondsLeft = (calculatedLeft < 0) ? 0 : calculatedLeft;
        } else {
          secondsLeft = -1;
        }

        if (user_data.timeoutValue > 0 && runningSeconds >= user_data.timeoutValue) {
          pumpState = false; timeoutTriggered = true;
          digitalWrite(pinRelay, LOW);
          Serial.println("КРИТИЧЕСКАЯ ОШИБКА: Превышен таймаут!");
        }
      } else {
        runningSeconds = 0; secondsLeft = 0; 
      }

      if (!timeoutTriggered) { digitalWrite(pinRelay, pumpState ? HIGH : LOW); } 
      else { digitalWrite(pinRelay, LOW); }
    } else {
      digitalWrite(pinRelay, pumpState ? HIGH : LOW);
    }
  }
  yield();
}
