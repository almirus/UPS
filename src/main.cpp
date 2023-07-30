#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <UniversalTelegramBot.h>

const char* ssid = "WIFI_NAME";        // Имя вашей Wi-Fi сети
const char* password = "WIFI_PASSWORD";  // Пароль для вашей Wi-Fi сети

const char* mqttServer = "192.168.0.170";  // IP-адрес MQTT-сервера
const char* mqttUsername = "mqtt";         // Логин MQTT-сервера
const char* mqttPassword = "mqtt";         // Пароль MQTT-сервера
const char* mqttClientID = "ups";          // Идентификатор клиента MQTT
const char* mqttTopic = "battery/charge";  // Топик MQTT для публикации данных о батарее
const char* mqttTopicH = "battery/health"; // Топик MQTT для публикации данных о температуре
const int mqttPort = 1883;                 // Порт MQTT-сервера

const char* botToken = ""; // Токен для телеграм бота
const char* chatId = "";    // чат бота
const unsigned long BOT_MTBS = 1000; // интервал обработки сообщений от бота
unsigned long botLasttime;

#define DHTPIN D4     // Пин, к которому подключен датчик DHT11
#define DHTTYPE DHT11 // Тип датчика DHT

const int batteryPin1 = D1;  // Пин входа для первого инвертированного сигнала 25%
const int batteryPin2 = D2;  // Пин входа для второго инвертированного сигнала 50%
const int batteryPin3 = D5;  // Пин входа для третьего инвертированного сигнала 70%
const int batteryPin4 = D6;  // Пин входа для четвертого инвертированного сигнала 100%
const int noPowerPin = D7;   // Пин входа для сигнала об отсутствии напряжения 220В

const int outputPin = D8;    // Пин вывода для отправки сигнала отключения линии USB

int battery=0;
bool noPower=false;
unsigned int unsuccessfullAttempts = 0;
float humidity;
float temperature;
const int maxTemp=41; // максимальная температура, при превышении будет уведомление

const int maxAttempt = 25;
const unsigned long MQTT_SEND_INTERVAL = 8000;
const unsigned long WARNING_INTERVAL = 1800000;
unsigned long previousMillis = 0;

X509List cert(TELEGRAM_CERTIFICATE_ROOT);
WiFiClientSecure secureClient;
WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);
ESP8266WebServer server(80);
UniversalTelegramBot bot(botToken, secureClient);

// перезагрузка устройства (прерывание питания USB) через Pololu Switch LV
void restartUSBdevice() {
  String html = "<html><head><meta charset=\"utf-8\"></head><body>";
  html += "Идет перезагрузка USB устройства...</body>";  

  bot.sendMessage(chatId, "Идет перезагрузка устройства подключенного к USB", "");

  digitalWrite(outputPin, LOW);
  delay(2000);
  digitalWrite(outputPin, HIGH);  

  server.send(200, "text/html", html);
}
// обработка сообщений от Телеграм бота
void handleNewMessages(int numNewMessages)
{
  for (int i = 0; i < numNewMessages; i++){
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    if (bot.messages[i].type == "callback_query"){
      text = bot.messages[i].text;
    } 
    if (text == "/health") {
      String text = "Состояние UPS:\n";
      if (noPower) text +="Сеть 220 отсутствует\n";
      else text +="Сеть 220 подключена\n";

      text +="Уровень заряда - "+String(battery)+"%\n";
      text +="Температура - "+String(temperature)+"\n";
      text +="Влажность - "+String(humidity)+"%";
      
      bot.sendMessage(chat_id, text, "Markdown");
    }
    if (text == "/rebootUSB"){
      restartUSBdevice();
    }    
  }
}

// Функция обработчика HTTP запроса
void handleRoot() {
  String html = "<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"15\"></head><title>Умный UPS</title><body>";
  html += "<h3>Уровень заряда:</h1>";
  html += "<p>" + String(battery) + "%</p>";
  if (noPower){
    html += "<h3 style=\"color:red;\">Нет сети 220В</h3>";
  }
  if (unsuccessfullAttempts>0)
    html += "<h3>Попытка " + String(unsuccessfullAttempts) + " соединиться с MQTT</h3>";
  else
    html += "<h3>Соединение с MQTT OK</h3>";
  html += "<p>Температура: " + String(temperature) + "</p>";
  html += "<p>Влажность: " + String(humidity) + "</p>";
  html +="<form action=\"/restart\">";
  html += "<button type=\"submit\">Перезагрузить устройство подключенное к USB</button>";
  html +="</form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void setup() {

  pinMode(batteryPin1, INPUT); // Инвертированный вход
  pinMode(batteryPin2, INPUT); // Инвертированный вход
  pinMode(batteryPin3, INPUT); // Инвертированный вход
  pinMode(batteryPin4, INPUT); // Инвертированный вход
  pinMode(noPowerPin, INPUT);  // Инвертированный вход
  
  pinMode(outputPin, OUTPUT);         // Установка пина вывода как выход
  digitalWrite(outputPin, HIGH);      // Включаем питание на USB

  Serial.begin(115200);
  delay(10);

  WiFi.begin(ssid, password);  // Подключение к Wi-Fi сети
  secureClient.setTrustAnchors(&cert);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("Connected to WiFi!");

  Serial.print("Retrieving time: ");
  configTime(0, 0, "pool.ntp.org"); // get UTC time via NTP
  time_t now = time(nullptr);
  while (now < 24 * 3600 || previousMillis>10000)
  {
    Serial.print(".");
    delay(100);
    now = time(nullptr);
    previousMillis++;
  }
  Serial.println(now);

  server.on("/", handleRoot);
  server.on("/restart", restartUSBdevice);
  server.begin();   
  client.setServer(mqttServer, mqttPort);
  IPAddress ip = WiFi.localIP();
  bot.sendMessage(chatId, "Умный UPS запущен. IP:"+ String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]), "");
  previousMillis = 0;
  unsuccessfullAttempts = 0;
}

void loop() {

  client.loop();
  
  server.handleClient();

  unsigned long currentMillis = millis();

  // обработка вх сообщений от бота
  if (currentMillis - botLasttime > BOT_MTBS) {
    botLasttime = currentMillis;
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages){
      Serial.println("got response");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
  } 
  // основная обработка 
  if (currentMillis - previousMillis > MQTT_SEND_INTERVAL) {    
    previousMillis = currentMillis;
    if (!client.connected()) {
      // Переподключение к MQTT-серверу при разрыве соединения
      while (!client.connected() && unsuccessfullAttempts < maxAttempt+5) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect(mqttClientID, mqttUsername, mqttPassword)) {
          Serial.println("connected");
          unsuccessfullAttempts=0;
        } else {
          unsuccessfullAttempts++;
          Serial.print("failed, rc=");
          Serial.print(client.state());
          Serial.println(" retrying in 5 seconds");
          delay(5000);
        }
      }
    }
    //_______________________________________________________________________
    //_____________________проверка уровня заряда UPS________________________
    //_______________________________________________________________________
    bool charge25 = !digitalRead(batteryPin1);
    bool charge50 = !digitalRead(batteryPin2);
    bool charge70 = !digitalRead(batteryPin3);
    bool charge100 = !digitalRead(batteryPin4);
    noPower = !digitalRead(noPowerPin);
  
    char payload[50];

    if (charge100) {
      battery = 100;
    } else if (charge70) {
      battery = 70;
    } else if (charge50) {
      battery = 50;
    } else if (charge25) {
      battery = 25;
    }
    snprintf(payload, sizeof(payload), "{\"power\":\"%d\",\"battery\":%d}",  noPower ? 0 : 1, battery);  // Форматирование данных в формат JSON
    Serial.println("Publishing data to MQTT...");
    client.publish(mqttTopic, payload);  // Отправка данных на MQTT-сервер
    Serial.println("Data published to MQTT");
    //_____________________________________________________
    //________________датчик температуры___________________
    //_____________________________________________________
    humidity = dht.readHumidity();  // Чтение влажности
    temperature = dht.readTemperature();  // Чтение температуры по Цельсию

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("Failed to read from DHT sensor!");
    }

    Serial.println("Publishing data to MQTT...");
    snprintf(payload, sizeof(payload), "{\"humidity\":%.2f,\"temperature\":%.2f}", humidity, temperature);  // Форматирование данных в формат JSON
    client.publish(mqttTopicH, payload);  // Отправка данных на MQTT-сервер
    Serial.println("Data published to MQTT");

    if (unsuccessfullAttempts > maxAttempt && (currentMillis - previousMillis) > WARNING_INTERVAL){
      String keyboardJson = "[[{ \"text\" : \"Перезагрузить USB устройство\", \"callback_data\" : \"/rebootUSB\" }]]";
      bot.sendMessageWithInlineKeyboard(chatId, "Похоже, что HA не отвечает", "", keyboardJson);
    }
    if (temperature > maxTemp && (currentMillis - previousMillis) > WARNING_INTERVAL){
      bot.sendMessage(chatId, "Внимание температура UPS превышает "+String(maxTemp)+" градусов", "");
    }
  }
  
}