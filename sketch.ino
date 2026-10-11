/*
  ============================================================================
   NEXORA  -  Modulo: ESTACAO METEOROLOGICA  (v1.0)
   Cidade: Alagoinhas - BA   (lat -12.1356, lon -38.4192, alt 158 m)
  ----------------------------------------------------------------------------
   O que faz:
   - Conecta no Wi-Fi e busca dados REAIS da internet (Open-Meteo, sem chave):
       * condicoes atuais, previsao de 12 horas e de 5 dias
       * nascer/por do sol, indice UV
       * qualidade do ar (AQI EUA, PM2.5, PM10)
   - Le sensores locais: DHT22 (temperatura/umidade) e BMP180 (pressao)
   - Mostra um painel com 6 paginas na TFT ILI9341 2,8" (colorida)
   - Mostra um resumo permanente na OLED SH1107 128x128
   - Paginas trocam sozinhas a cada 10 s; botoes avancam/voltam
   - LED RGB indica o estado:  verde = tudo ok | amarelo = atencao |
                               vermelho piscando = alerta | azul = sem Wi-Fi
  ----------------------------------------------------------------------------
   Ligacoes (ESP32 DevKit C):
     TFT ILI9341: CS=15  DC=2  RST=4  MOSI=23  SCK=18  MISO=19  VCC/LED=3V3
     I2C (OLED SH1107 + BMP180): SDA=21  SCL=22
     DHT22: dados=13
     Botoes (ate GND, pull-up interno): PROXIMA=32  ANTERIOR=33
     LED RGB catodo comum (com resistor 220R em cada cor): R=25 G=26 B=27
  ============================================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>
#include <time.h>
#include <math.h>

// ============================== CONFIGURACAO ===============================
// No Wokwi a rede e sempre "Wokwi-GUEST" sem senha (canal 6 conecta mais rapido).
// Na montagem fisica, troque pelo nome/senha da sua rede e use canal 0 (auto).
const char* WIFI_SSID    = "Wokwi-GUEST";
const char* WIFI_PASS    = "";
const int   WIFI_CHANNEL = 6;

const char* CITY_NAME  = "ALAGOINHAS-BA";
const float LATITUDE   = -12.1356;
const float LONGITUDE  = -38.4192;
const float ALTITUDE_M = 158.0;          // altitude da cidade (para corrigir a pressao)
const long  GMT_OFFSET = -3 * 3600;      // fuso de Brasilia/Bahia (UTC-3)

const unsigned long WEATHER_EVERY = 10UL * 60UL * 1000UL;  // previsao: 10 min
const unsigned long AIR_EVERY     = 30UL * 60UL * 1000UL;  // ar: 30 min
const unsigned long RETRY_EVERY   = 20UL * 1000UL;         // nova tentativa se falhar
const unsigned long SENSOR_EVERY  = 5000;                  // sensores locais: 5 s
const unsigned long PAGE_EVERY    = 10000;                 // troca automatica: 10 s
const unsigned long MANUAL_HOLD   = 30000;                 // pausa apos apertar botao

// ================================= PINOS ===================================
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define I2C_SDA  21
#define I2C_SCL  22
#define DHT_PIN  13
#define BTN_NEXT 32
#define BTN_PREV 33
#define LED_R    25
#define LED_G    26
#define LED_B    27

// =============================== OBJETOS ===================================
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// O Wokwi simula o controlador SH1107 "generico". Se comprar a OLED Grove 1.12"
// da Seeed e a imagem aparecer deslocada, mude para 1.
#define USE_SEEED_OLED 0
#if USE_SEEED_OLED
U8G2_SH1107_SEEED_128X128_F_HW_I2C oled(U8G2_R0);
#else
U8G2_SH1107_128X128_F_HW_I2C oled(U8G2_R0);
#endif

DHT dht(DHT_PIN, DHT22);
Adafruit_BMP085 bmp;            // a biblioteca do BMP085 serve para o BMP180

// ======================= CORES (formato RGB565) ============================
#define C_BG     0x0863   // fundo azul-escuro
#define C_CARD   0x1906   // cartoes
#define C_TEXT   0xFFFF   // branco
#define C_MUTED  0x8CB5   // cinza azulado
#define C_ACCENT 0x065F   // ciano
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BLUE   0x3DBF
#define C_PURPLE 0xA81F
#define C_MAROON 0x7800
#define C_CLOUD  0xBDF7
#define C_DCLOUD 0x6B4D
#define C_MOON   0xDEDB

// ===================== PROTOTIPOS (funcoes com valor padrao) ===============
// Declarar aqui permite usar as funcoes antes de defini-las e deixar
// parametros opcionais (ex.: a cor de fundo assume C_BG se nao for informada).
void txt(int x, int y, const String& s, uint8_t size, uint16_t fg, uint16_t bg = C_BG);
void txtC(int cx, int y, const String& s, uint8_t size, uint16_t fg, uint16_t bg = C_BG);
int  tempAt(int x, int y, float v, uint8_t size, uint16_t fg, bool withC = true, uint16_t bg = C_BG);
void weatherIcon(int cx, int cy, int s, int code, bool day, uint16_t bg = C_BG);
void addAlert(int lvl, const char* fmt, float v = 0);
void drawClock();

// ============================ DADOS GUARDADOS ==============================
struct Weather {
  bool  ok = false;
  float temp, feels, hum, precip, cloud, pmsl, psurf, wind, windDir, gust, uv;
  int   code = 0, isDay = 1;
  int   nH = 0;                      // previsao horaria (12 horas)
  int   hHour[12]; float hTemp[12]; int hProb[12]; int hCode[12];
  int   nD = 0;                      // previsao diaria (5 dias)
  char  dDate[5][11]; float dMax[5], dMin[5], dRain[5], dUV[5];
  int   dProb[5], dCode[5];
  char  sunrise[6] = "--:--", sunset[6] = "--:--";
  char  updated[6] = "--:--";
} wx;

struct Air {
  bool ok = false;
  int aqi = 0; float pm25 = 0, pm10 = 0;
} air;

struct Local {
  bool  dhtOk = false, bmpOk = false;
  float t = NAN, h = NAN;          // DHT22
  float tb = NAN, p = NAN, p0 = NAN; // BMP180: temp, pressao absoluta e ao nivel do mar (hPa)
  float dew = NAN;                 // ponto de orvalho calculado
} loc;

struct Alert { int level; char msg[44]; };
Alert alerts[8];
int   nAlerts = 0;
int   alertLevel = 0;              // 0 ok, 1 atencao, 2 alerta

// ============================ CONTROLE DE TELA =============================
const int NUM_PAGES = 6;
const char* PAGE_TITLE[NUM_PAGES] = {
  "AGORA", "PROXIMAS 12H", "PROX. 5 DIAS", "SOL E UV", "AR E ALERTAS", "SENSORES"
};
int  page = 0;
bool needRedraw = true;
unsigned long lastPageChange = 0, lastManual = 0;
unsigned long lastWeather = 0, lastWeatherTry = 0, lastAir = 0, lastAirTry = 0;
unsigned long lastSensor = 0, lastSecond = 0, lastWifiTry = 0;
bool firstWeather = true, firstAir = true;
bool blinkState = false;

// ================================ UTILIDADES ===============================
bool wifiOk() { return WiFi.status() == WL_CONNECTED; }

// Hora atual (retorna false se o relogio ainda nao foi sincronizado pelo NTP)
bool nowTime(struct tm &ti) { return getLocalTime(&ti, 5); }

String hhmm() {
  struct tm ti;
  if (!nowTime(ti)) return "--:--";
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
  return String(b);
}

// Dia da semana a partir de "AAAA-MM-DD" (algoritmo de Sakamoto). 0 = domingo
int weekdayOf(const char* d) {
  int y = atoi(d), m = atoi(d + 5), day = atoi(d + 8);
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + day) % 7;
}

int minutesOf(const char* hm) { return atoi(hm) * 60 + atoi(hm + 3); }  // "05:21" -> 321

const char* windDirName(float deg) {
  static const char* d[] = {"N", "NE", "L", "SE", "S", "SO", "O", "NO"};
  return d[((int)((deg + 22.5) / 45.0)) % 8];
}

const char* weatherText(int c) {
  switch (c) {
    case 0: return "Ceu limpo";
    case 1: return "Poucas nuvens";
    case 2: return "Parc. nublado";
    case 3: return "Nublado";
    case 45: case 48: return "Neblina";
    case 51: case 53: case 55: return "Garoa";
    case 56: case 57: return "Garoa gelada";
    case 61: return "Chuva fraca";
    case 63: return "Chuva moderada";
    case 65: return "Chuva forte";
    case 66: case 67: return "Chuva gelada";
    case 71: case 73: case 75: case 77: case 85: case 86: return "Neve";
    case 80: return "Pancadas fracas";
    case 81: return "Pancadas de chuva";
    case 82: return "Pancadas fortes";
    case 95: return "Trovoada";
    case 96: case 99: return "Trovoada c/granizo";
    case 97: return "Trovoada forte";
  }
  return "---";
}

// Ponto de orvalho (formula de Magnus): temperatura em que o ar satura
float dewPoint(float t, float rh) {
  const float b = 17.62, c = 243.12;
  float g = log(rh / 100.0) + (b * t) / (c + t);
  return (c * g) / (b - g);
}

// Pressao reduzida ao nivel do mar (formula barometrica internacional)
float seaLevel(float p_hPa, float alt) { return p_hPa / pow(1.0 - alt / 44330.0, 5.255); }

void uvInfo(float uv, const char*& name, uint16_t& col) {
  if (uv < 3)       { name = "Baixo";      col = C_GREEN;  }
  else if (uv < 6)  { name = "Moderado";   col = C_YELLOW; }
  else if (uv < 8)  { name = "Alto";       col = C_ORANGE; }
  else if (uv < 11) { name = "Muito alto"; col = C_RED;    }
  else              { name = "Extremo";    col = C_PURPLE; }
}

void aqiInfo(int aqi, const char*& name, uint16_t& col) {
  if (aqi <= 50)       { name = "Bom";               col = C_GREEN;  }
  else if (aqi <= 100) { name = "Moderado";          col = C_YELLOW; }
  else if (aqi <= 150) { name = "Ruim p/ sensiveis"; col = C_ORANGE; }
  else if (aqi <= 200) { name = "Ruim";              col = C_RED;    }
  else if (aqi <= 300) { name = "Muito ruim";        col = C_PURPLE; }
  else                 { name = "Perigoso";          col = C_MAROON; }
}

// ============================ INTERNET (HTTPS) =============================
bool httpGetJson(const String& url, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();     // simplificacao: nao valida o certificado (ver PDF)
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) { Serial.println("[HTTP] begin falhou"); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[HTTP] erro %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body);
  if (err) { Serial.printf("[JSON] erro: %s\n", err.c_str()); return false; }
  Serial.printf("[HTTP] ok, %u bytes\n", body.length());
  return true;
}

bool fetchWeather() {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(LATITUDE, 4) +
               "&longitude=" + String(LONGITUDE, 4) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
               "precipitation,weather_code,cloud_cover,pressure_msl,surface_pressure,"
               "wind_speed_10m,wind_direction_10m,wind_gusts_10m,uv_index"
               "&hourly=temperature_2m,precipitation_probability,weather_code"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,"
               "uv_index_max,precipitation_probability_max,precipitation_sum"
               "&timezone=America%2FBahia&forecast_days=5&forecast_hours=12";
  Serial.println("[CLIMA] buscando previsao...");
  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  JsonObject c = doc["current"];
  wx.temp    = c["temperature_2m"] | NAN;
  wx.hum     = c["relative_humidity_2m"] | NAN;
  wx.feels   = c["apparent_temperature"] | NAN;
  wx.isDay   = c["is_day"] | 1;
  wx.precip  = c["precipitation"] | 0.0f;
  wx.code    = c["weather_code"] | 0;
  wx.cloud   = c["cloud_cover"] | 0.0f;
  wx.pmsl    = c["pressure_msl"] | NAN;
  wx.psurf   = c["surface_pressure"] | NAN;
  wx.wind    = c["wind_speed_10m"] | 0.0f;
  wx.windDir = c["wind_direction_10m"] | 0.0f;
  wx.gust    = c["wind_gusts_10m"] | 0.0f;
  wx.uv      = c["uv_index"] | 0.0f;

  JsonObject h = doc["hourly"];
  JsonArray ht = h["time"];
  wx.nH = min((int)ht.size(), 12);
  for (int i = 0; i < wx.nH; i++) {
    const char* t = ht[i] | "0000-00-00T00:00";
    wx.hHour[i] = atoi(t + 11);
    wx.hTemp[i] = h["temperature_2m"][i] | 0.0f;
    wx.hProb[i] = h["precipitation_probability"][i] | 0;
    wx.hCode[i] = h["weather_code"][i] | 0;
  }

  JsonObject d = doc["daily"];
  JsonArray dt = d["time"];
  wx.nD = min((int)dt.size(), 5);
  for (int i = 0; i < wx.nD; i++) {
    strlcpy(wx.dDate[i], dt[i] | "0000-00-00", sizeof(wx.dDate[i]));
    wx.dMax[i]  = d["temperature_2m_max"][i] | 0.0f;
    wx.dMin[i]  = d["temperature_2m_min"][i] | 0.0f;
    wx.dRain[i] = d["precipitation_sum"][i] | 0.0f;
    wx.dUV[i]   = d["uv_index_max"][i] | 0.0f;
    wx.dProb[i] = d["precipitation_probability_max"][i] | 0;
    wx.dCode[i] = d["weather_code"][i] | 0;
  }
  const char* sr = d["sunrise"][0] | "0000-00-00T--:--";
  const char* ss = d["sunset"][0]  | "0000-00-00T--:--";
  strlcpy(wx.sunrise, sr + 11, sizeof(wx.sunrise));
  strlcpy(wx.sunset,  ss + 11, sizeof(wx.sunset));

  // se o NTP ainda nao sincronizou, usa o horario que veio da API
  String hm = hhmm();
  if (hm == "--:--") { const char* ct = c["time"] | "0000-00-00T--:--"; hm = String(ct + 11); }
  strlcpy(wx.updated, hm.c_str(), sizeof(wx.updated));

  wx.ok = true;
  Serial.printf("[CLIMA] %.1f C, %s, umid %.0f%%, UV %.1f\n", wx.temp, weatherText(wx.code), wx.hum, wx.uv);
  return true;
}

bool fetchAir() {
  String url = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + String(LATITUDE, 4) +
               "&longitude=" + String(LONGITUDE, 4) +
               "&current=us_aqi,pm10,pm2_5&timezone=America%2FBahia";
  Serial.println("[AR] buscando qualidade do ar...");
  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;
  air.aqi  = doc["current"]["us_aqi"] | 0;
  air.pm25 = doc["current"]["pm2_5"] | 0.0f;
  air.pm10 = doc["current"]["pm10"] | 0.0f;
  air.ok = true;
  Serial.printf("[AR] AQI %d, PM2.5 %.1f, PM10 %.1f\n", air.aqi, air.pm25, air.pm10);
  return true;
}

// ============================ SENSORES LOCAIS ==============================
void readSensors() {
  float t = dht.readTemperature(), h = dht.readHumidity();
  loc.dhtOk = !isnan(t) && !isnan(h);
  if (loc.dhtOk) { loc.t = t; loc.h = h; loc.dew = dewPoint(t, h); }

  if (loc.bmpOk) {
    loc.tb = bmp.readTemperature();
    loc.p  = bmp.readPressure() / 100.0;          // Pa -> hPa
    loc.p0 = seaLevel(loc.p, ALTITUDE_M);
  }
}

// ================================ ALERTAS ==================================
void addAlert(int lvl, const char* fmt, float v) {
  if (nAlerts >= 8) return;
  alerts[nAlerts].level = lvl;
  snprintf(alerts[nAlerts].msg, sizeof(alerts[nAlerts].msg), fmt, v);
  nAlerts++;
  if (lvl > alertLevel) alertLevel = lvl;
}

void evaluateAlerts() {
  nAlerts = 0; alertLevel = 0;
  if (!wifiOk()) addAlert(1, "Sem Wi-Fi: dados podem estar antigos");
  if (wx.ok) {
    if (wx.code >= 95)                       addAlert(2, "Trovoada agora!");
    else if (wx.code == 65 || wx.code == 82) addAlert(2, "Chuva forte agora");

    if (wx.feels >= 38)      addAlert(2, "Calor extremo: sensacao %.0f C", wx.feels);
    else if (wx.feels >= 33) addAlert(1, "Calor forte: sensacao %.0f C", wx.feels);

    if (wx.uv >= 8)                         addAlert(2, "UV muito alto agora (%.0f)", wx.uv);
    else if (wx.nD > 0 && wx.dUV[0] >= 8)   addAlert(1, "UV maximo hoje: %.0f - use protetor", wx.dUV[0]);

    if (wx.gust >= 60)      addAlert(2, "Rajadas fortes: %.0f km/h", wx.gust);
    else if (wx.gust >= 40) addAlert(1, "Ventania: rajadas %.0f km/h", wx.gust);

    int maxProb = 0;
    for (int i = 0; i < min(wx.nH, 6); i++) maxProb = max(maxProb, wx.hProb[i]);
    if (maxProb >= 70) addAlert(1, "Chuva provavel nas prox. 6h (%.0f%%)", maxProb);

    if (wx.nD > 0 && wx.dRain[0] >= 30) addAlert(2, "Muita chuva prevista hoje: %.0f mm", wx.dRain[0]);
  }
  if (air.ok) {
    if (air.aqi > 150)      addAlert(2, "Qualidade do ar ruim (AQI %.0f)", air.aqi);
    else if (air.aqi > 100) addAlert(1, "Ar ruim p/ sensiveis (AQI %.0f)", air.aqi);
  }
  if (loc.dhtOk && loc.t >= 35) addAlert(1, "Casa muito quente: %.1f C", loc.t);
}

// =============================== LED RGB ===================================
void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R, r); digitalWrite(LED_G, g); digitalWrite(LED_B, b);
}

void updateLed() {
  if (!wifiOk() && !wx.ok)  setLed(0, 0, blinkState);          // azul piscando
  else if (alertLevel == 2) setLed(blinkState, 0, 0);          // vermelho piscando
  else if (alertLevel == 1) setLed(1, 1, 0);                   // amarelo (R+G)
  else                      setLed(0, 1, 0);                   // verde
}

// ======================= DESENHO: AJUDANTES DA TFT =========================
void txt(int x, int y, const String& s, uint8_t size, uint16_t fg, uint16_t bg) {
  tft.setTextSize(size); tft.setTextColor(fg, bg); tft.setCursor(x, y); tft.print(s);
}
int  txtW(const String& s, uint8_t size) { return s.length() * 6 * size; }
void txtC(int cx, int y, const String& s, uint8_t size, uint16_t fg, uint16_t bg) {
  txt(cx - txtW(s, size) / 2, y, s, size, fg, bg);
}

// Escreve uma temperatura com o simbolo de grau desenhado (a fonte nao tem "°")
int tempAt(int x, int y, float v, uint8_t size, uint16_t fg, bool withC, uint16_t bg) {
  String s = isnan(v) ? "--" : String((int)round(v));
  txt(x, y, s, size, fg, bg);
  int w = txtW(s, size);
  int r = max(2, (int)size);
  tft.drawCircle(x + w + r + 1, y + r, r, fg);
  if (size >= 3) tft.drawCircle(x + w + r + 1, y + r, r - 1, fg);
  w += 2 * r + 4;
  if (withC) { txt(x + w, y, "C", size >= 4 ? 3 : size, fg, bg); w += 6 * (size >= 4 ? 3 : size); }
  return w;
}

// --------------------------- icones do tempo -------------------------------
void iconSun(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, C_YELLOW);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    tft.drawLine(cx + cos(a) * (r + 3), cy + sin(a) * (r + 3),
                 cx + cos(a) * (r + 3 + r / 2), cy + sin(a) * (r + 3 + r / 2), C_YELLOW);
  }
}
void iconMoon(int cx, int cy, int r, uint16_t bg) {
  tft.fillCircle(cx, cy, r, C_MOON);
  tft.fillCircle(cx + r / 2, cy - r / 3, r, bg);
}
void iconCloud(int cx, int cy, int s, uint16_t col) {   // s = meia largura
  tft.fillCircle(cx - s / 2, cy, s / 3, col);
  tft.fillCircle(cx + s / 2, cy, s / 3, col);
  tft.fillCircle(cx - s / 8, cy - s / 4, s / 2 - 1, col);
  tft.fillRect(cx - s / 2, cy, s, s / 3 + 1, col);
}

// Escolhe o icone pelo codigo WMO. s ~ meia largura do icone
void weatherIcon(int cx, int cy, int s, int code, bool day, uint16_t bg) {
  if (code <= 1) {
    if (day) iconSun(cx, cy, s / 2); else iconMoon(cx, cy, s / 2, bg);
    if (code == 1) iconCloud(cx + s / 2, cy + s / 2, s / 2, C_CLOUD);
  } else if (code == 2) {
    if (day) iconSun(cx - s / 3, cy - s / 3, s / 3); else iconMoon(cx - s / 3, cy - s / 3, s / 3, bg);
    iconCloud(cx + s / 6, cy + s / 6, s * 3 / 4, C_CLOUD);
  } else if (code == 3) {
    iconCloud(cx, cy, s, C_CLOUD);
  } else if (code == 45 || code == 48) {
    for (int i = -1; i <= 1; i++) tft.fillRect(cx - s, cy + i * s / 2, 2 * s, max(2, s / 8), C_CLOUD);
  } else if (code >= 95) {
    iconCloud(cx, cy - s / 4, s, C_DCLOUD);
    tft.fillTriangle(cx, cy, cx - s / 3, cy + s / 2, cx + s / 8, cy + s / 2, C_YELLOW);
    tft.fillTriangle(cx - s / 8, cy + s / 2, cx + s / 3, cy + s / 2, cx - s / 6, cy + s, C_YELLOW);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    iconCloud(cx, cy - s / 4, s, C_CLOUD);
    for (int i = -1; i <= 1; i++) tft.fillCircle(cx + i * s / 2, cy + s / 2 + abs(i) * 3, max(1, s / 10), C_TEXT);
  } else {  // garoa / chuva / pancadas
    iconCloud(cx, cy - s / 4, s, code >= 63 ? C_DCLOUD : C_CLOUD);
    int n = (code == 65 || code == 82) ? 4 : 3;
    for (int i = 0; i < n; i++) {
      int x = cx - s / 2 + i * s / (n - 1);
      tft.drawLine(x, cy + s / 3, x - s / 6, cy + s * 3 / 4, C_BLUE);
      tft.drawLine(x + 1, cy + s / 3, x + 1 - s / 6, cy + s * 3 / 4, C_BLUE);
    }
  }
}

// ======================= DESENHO: MOLDURA DA TFT ===========================
void drawHeader() {
  tft.fillRect(0, 0, 320, 26, C_CARD);
  txt(8, 6, CITY_NAME, 2, C_TEXT, C_CARD);
  txt(170, 10, PAGE_TITLE[page], 1, C_ACCENT, C_CARD);
  drawClock();
}

void drawClock() {
  txt(252, 6, hhmm(), 2, C_TEXT, C_CARD);
}

void drawFooter() {
  tft.fillRect(0, 226, 320, 14, C_BG);
  txt(6, 229, String("Atual. ") + wx.updated, 1, C_MUTED);
  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 160 - (NUM_PAGES * 12) / 2 + i * 12 + 6;
    if (i == page) tft.fillCircle(x, 232, 4, C_ACCENT); else tft.drawCircle(x, 232, 3, C_MUTED);
  }
  if (wifiOk()) txt(272, 229, "WiFi OK", 1, C_GREEN);
  else          txt(266, 229, "OFFLINE", 1, C_RED);
}

void card(int x, int y, int w, int h, const char* label, const String& value, uint16_t vcol = C_TEXT) {
  tft.fillRoundRect(x, y, w, h, 6, C_CARD);
  txt(x + 6, y + 5, label, 1, C_MUTED, C_CARD);
  txt(x + 6, y + 18, value, 2, vcol, C_CARD);
}

void noData(const char* what) {
  txtC(160, 110, what, 2, C_MUTED);
  txtC(160, 135, wifiOk() ? "Buscando dados..." : "Conectando ao Wi-Fi...", 1, C_MUTED);
}

// =========================== PAGINA 0: AGORA ===============================
void pageNow() {
  if (!wx.ok) { noData("Sem dados ainda"); return; }
  weatherIcon(60, 80, 44, wx.code, wx.isDay);
  tempAt(122, 38, wx.temp, 6, C_TEXT);
  txt(122, 94, weatherText(wx.code), 2, C_ACCENT);
  if (wx.nD > 0) {
    txt(122, 116, "Max", 1, C_MUTED); tempAt(144, 113, wx.dMax[0], 2, C_ORANGE, false);
    txt(200, 116, "Min", 1, C_MUTED); tempAt(222, 113, wx.dMin[0], 2, C_BLUE, false);
  }
  const int y1 = 138, y2 = 182, w = 100, h = 40;
  card(6,   y1, w, h, "Sensacao", String((int)round(wx.feels)) + " C", wx.feels >= 33 ? C_ORANGE : C_TEXT);
  card(110, y1, w, h, "Umidade", String((int)round(wx.hum)) + " %");
  card(214, y1, w, h, "Vento km/h", String((int)round(wx.wind)) + " " + windDirName(wx.windDir));
  card(6,   y2, w, h, "Rajadas km/h", String((int)round(wx.gust)), wx.gust >= 40 ? C_ORANGE : C_TEXT);
  card(110, y2, w, h, "Pressao hPa", String((int)round(wx.pmsl)));
  card(214, y2, w, h, "Nuvens", String((int)round(wx.cloud)) + " %");
}

// ===================== PAGINA 1: PROXIMAS 12 HORAS =========================
void pageHours() {
  if (!wx.ok || wx.nH < 2) { noData("Sem previsao ainda"); return; }
  const int x0 = 34, x1 = 312, yTop = 48, yBot = 172;
  float tMin = 99, tMax = -99;
  for (int i = 0; i < wx.nH; i++) { tMin = min(tMin, wx.hTemp[i]); tMax = max(tMax, wx.hTemp[i]); }
  tMin = floor(tMin - 1); tMax = ceil(tMax + 1);
  float step = (float)(x1 - x0) / wx.nH;

  // eixos e legenda
  tft.drawFastHLine(x0, yBot, x1 - x0, C_MUTED);
  tft.drawFastVLine(x0, yTop, yBot - yTop, C_MUTED);
  tempAt(4, yTop - 4, tMax, 1, C_ORANGE, false);
  tempAt(4, yBot - 8, tMin, 1, C_ORANGE, false);
  txt(x0 + 4, 32, "Temperatura", 1, C_ORANGE);
  txt(x0 + 90, 32, "Chance de chuva (%)", 1, C_BLUE);

  // barras de chance de chuva (0 a 100% ocupa a altura toda)
  for (int i = 0; i < wx.nH; i++) {
    int bh = (int)((yBot - yTop) * wx.hProb[i] / 100.0);
    int bx = x0 + (int)(i * step) + 3;
    if (bh > 0) tft.fillRect(bx, yBot - bh, (int)step - 6, bh, 0x2B3F);
  }
  // linha de temperatura
  int px = 0, py = 0;
  for (int i = 0; i < wx.nH; i++) {
    int x = x0 + (int)(i * step + step / 2);
    int y = yBot - (int)((wx.hTemp[i] - tMin) / (tMax - tMin) * (yBot - yTop));
    if (i > 0) { tft.drawLine(px, py, x, y, C_ORANGE); tft.drawLine(px, py + 1, x, y + 1, C_ORANGE); }
    tft.fillCircle(x, y, 3, C_ORANGE);
    String v = String((int)round(wx.hTemp[i]));
    txtC(x, y - 13, v, 1, C_TEXT, C_BG);
    char hb[4]; snprintf(hb, sizeof(hb), "%02d", wx.hHour[i]);
    txtC(x, yBot + 5, hb, 1, C_MUTED);
    px = x; py = y;
  }
  txt(x1 - 6, yBot + 5, "h", 1, C_MUTED);

  // resumo em texto
  int best = 0;
  for (int i = 1; i < wx.nH; i++) if (wx.hProb[i] > wx.hProb[best]) best = i;
  char line[48];
  if (wx.hProb[best] >= 30) snprintf(line, sizeof(line), "Maior chance de chuva: %02dh (%d%%)", wx.hHour[best], wx.hProb[best]);
  else                      snprintf(line, sizeof(line), "Pouca chance de chuva nas proximas 12h");
  txt(8, 190, line, 1, wx.hProb[best] >= 60 ? C_YELLOW : C_TEXT);
  snprintf(line, sizeof(line), "Variacao: %d a %d C", (int)round(tMin + 1), (int)round(tMax - 1));
  txt(8, 206, line, 1, C_MUTED);
}

// ======================= PAGINA 2: PROXIMOS 5 DIAS =========================
void pageDays() {
  if (!wx.ok || wx.nD == 0) { noData("Sem previsao ainda"); return; }
  static const char* dn[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"};
  txt(118, 32, "Max", 1, C_ORANGE); txt(166, 32, "Min", 1, C_BLUE);
  txt(214, 32, "Chuva", 1, C_BLUE); txt(266, 32, "Total", 1, C_MUTED);
  for (int i = 0; i < wx.nD; i++) {
    int y = 44 + i * 36;
    uint16_t bg = (i % 2 == 0) ? C_CARD : C_BG;
    tft.fillRect(0, y, 320, 36, bg);
    String name = (i == 0) ? "Hoje" : dn[weekdayOf(wx.dDate[i])];
    txt(8, y + 4, name, 2, C_TEXT, bg);
    char dm[6]; snprintf(dm, sizeof(dm), "%.2s/%.2s", wx.dDate[i] + 8, wx.dDate[i] + 5);
    txt(8, y + 23, dm, 1, C_MUTED, bg);
    weatherIcon(85, y + 16, 14, wx.dCode[i], true, bg);
    tempAt(118, y + 10, wx.dMax[i], 2, C_ORANGE, false, bg);
    tempAt(166, y + 10, wx.dMin[i], 2, C_BLUE, false, bg);
    txt(214, y + 10, String(wx.dProb[i]) + "%", 2, wx.dProb[i] >= 60 ? C_YELLOW : C_TEXT, bg);
    txt(266, y + 14, String(wx.dRain[i], 1) + "mm", 1, C_MUTED, bg);
  }
}

// ========================= PAGINA 3: SOL E UV ==============================
void pageSun() {
  if (!wx.ok) { noData("Sem dados ainda"); return; }
  // ---- arco do sol (lado esquerdo)
  const int cx = 82, cy = 150, r = 66;
  for (int a = 0; a <= 180; a += 6) {
    float rad = a * PI / 180.0;
    tft.fillCircle(cx - cos(rad) * r, cy - sin(rad) * r, 1, C_MUTED);
  }
  tft.drawFastHLine(cx - r - 10, cy, 2 * r + 20, C_MUTED);
  int rise = minutesOf(wx.sunrise), set = minutesOf(wx.sunset);
  struct tm ti;
  if (nowTime(ti)) {
    int nowM = ti.tm_hour * 60 + ti.tm_min;
    if (nowM >= rise && nowM <= set) {
      float frac = (float)(nowM - rise) / (set - rise);       // 0 = nasceu, 1 = se pos
      float rad = frac * PI;
      iconSun(cx - cos(rad) * r, cy - sin(rad) * r, 7);
    } else {
      iconMoon(cx, cy - 30, 10, C_BG);
    }
  }
  txt(cx - r - 14, cy + 8, wx.sunrise, 2, C_YELLOW);
  txt(cx + r - 46, cy + 8, wx.sunset, 2, C_ORANGE);
  txt(cx - r - 14, cy + 28, "Nascer", 1, C_MUTED);
  txt(cx + r - 46, cy + 28, "Por do sol", 1, C_MUTED);
  int len = set - rise;
  char b[24]; snprintf(b, sizeof(b), "Dia: %dh%02dmin", len / 60, len % 60);
  txtC(cx, cy + 46, b, 1, C_TEXT);

  // ---- indice UV (lado direito)
  const char* uvName; uint16_t uvCol;
  uvInfo(wx.uv, uvName, uvCol);
  tft.fillRoundRect(180, 34, 132, 104, 8, C_CARD);
  txt(190, 40, "UV AGORA", 1, C_MUTED, C_CARD);
  txt(190, 54, String(wx.uv, 1), 4, uvCol, C_CARD);
  txt(190, 90, uvName, 2, uvCol, C_CARD);
  if (wx.nD > 0) txt(190, 116, String("Max hoje: ") + String(wx.dUV[0], 1), 1, C_TEXT, C_CARD);

  const char* tip;
  float uvRef = max(wx.uv, wx.nD > 0 ? wx.dUV[0] : 0);
  if (uvRef < 3) tip = "Sem cuidado especial";
  else if (uvRef < 6) tip = "Protetor e bone";
  else if (uvRef < 8) tip = "Protetor, evite 10h-16h";
  else tip = "Evite sol 10h-16h!";
  txt(182, 146, tip, 1, C_TEXT);

  // ---- escala de UV de 0 a 12
  const int sx = 182, sy = 168, sw = 130;
  const float lim[] = {0, 3, 6, 8, 11, 12};
  const uint16_t col[] = {C_GREEN, C_YELLOW, C_ORANGE, C_RED, C_PURPLE};
  for (int i = 0; i < 5; i++) {
    int xa = sx + lim[i] / 12.0 * sw, xb = sx + lim[i + 1] / 12.0 * sw;
    tft.fillRect(xa, sy, xb - xa, 10, col[i]);
  }
  int mx = sx + min(wx.uv, 12.0f) / 12.0 * sw;
  tft.fillTriangle(mx, sy + 12, mx - 5, sy + 20, mx + 5, sy + 20, C_TEXT);
  txt(sx, sy + 24, "0", 1, C_MUTED); txt(sx + sw - 12, sy + 24, "12", 1, C_MUTED);
}

// ======================== PAGINA 4: AR E ALERTAS ===========================
void pageAir() {
  tft.fillRoundRect(6, 32, 308, 74, 8, C_CARD);
  txt(14, 38, "QUALIDADE DO AR (AQI EUA)", 1, C_MUTED, C_CARD);
  if (air.ok) {
    const char* name; uint16_t col;
    aqiInfo(air.aqi, name, col);
    txt(14, 54, String(air.aqi), 4, col, C_CARD);
    txt(110, 54, name, 2, col, C_CARD);
    txt(110, 78, String("PM2.5: ") + String(air.pm25, 1) + " ug/m3", 1, C_TEXT, C_CARD);
    txt(110, 90, String("PM10 : ") + String(air.pm10, 1) + " ug/m3", 1, C_TEXT, C_CARD);
  } else {
    txt(14, 60, "Aguardando dados...", 2, C_MUTED, C_CARD);
  }

  txt(8, 114, "ALERTAS ATIVOS", 2, C_TEXT);
  if (nAlerts == 0) {
    tft.fillCircle(14, 146, 5, C_GREEN);
    txt(26, 142, "Nenhum alerta. Tudo tranquilo!", 1, C_GREEN);
    return;
  }
  for (int i = 0; i < min(nAlerts, 6); i++) {
    int y = 138 + i * 15;
    uint16_t c = alerts[i].level == 2 ? C_RED : C_YELLOW;
    tft.fillCircle(14, y + 4, 4, c);
    txt(26, y, alerts[i].msg, 1, c);
  }
}

// ===================== PAGINA 5: SENSORES LOCAIS ===========================
void row(int y, const char* label, const String& local, const String& net, const String& diff) {
  txt(8, y + 4, label, 1, C_MUTED);
  txt(120, y, local, 2, C_TEXT);
  txt(200, y, net, 2, C_ACCENT);
  txt(280, y + 4, diff, 1, C_MUTED);
}
String f1(float v) { return isnan(v) ? String("--") : String(v, 1); }
String fd(float a, float b) {
  if (isnan(a) || isnan(b)) return "";
  float d = a - b; return (d >= 0 ? "+" : "") + String(d, 1);
}

void pageSensors() {
  txt(120, 32, "CASA", 1, C_TEXT);
  txt(200, 32, "INTERNET", 1, C_ACCENT);
  txt(280, 32, "DIF.", 1, C_MUTED);
  tft.drawFastHLine(6, 44, 308, C_CARD);
  float netDew = (wx.ok) ? dewPoint(wx.temp, wx.hum) : NAN;
  row(52,  "Temperatura C", f1(loc.t), wx.ok ? f1(wx.temp) : "--", fd(loc.t, wx.temp));
  row(80,  "Umidade %", loc.dhtOk ? String((int)round(loc.h)) : "--", wx.ok ? String((int)round(wx.hum)) : "--", fd(loc.h, wx.hum));
  row(108, "Orvalho C", f1(loc.dew), f1(netDew), fd(loc.dew, netDew));
  row(136, "Pressao mar", loc.bmpOk ? String((int)round(loc.p0)) : "--", wx.ok ? String((int)round(wx.pmsl)) : "--", fd(loc.p0, wx.pmsl));
  row(164, "Pressao local", loc.bmpOk ? String((int)round(loc.p)) : "--", wx.ok ? String((int)round(wx.psurf)) : "--", fd(loc.p, wx.psurf));
  tft.drawFastHLine(6, 188, 308, C_CARD);
  txt(8, 196, String("BMP180 temp: ") + f1(loc.tb) + " C   Altitude: " + String((int)ALTITUDE_M) + " m", 1, C_MUTED);
  String st = String("DHT22: ") + (loc.dhtOk ? "OK" : "ERRO") + "   BMP180: " + (loc.bmpOk ? "OK" : "ERRO");
  txt(8, 210, st, 1, (loc.dhtOk && loc.bmpOk) ? C_GREEN : C_RED);
}

// =========================== DESENHA A PAGINA ==============================
void drawPage() {
  tft.fillScreen(C_BG);
  drawHeader();
  switch (page) {
    case 0: pageNow(); break;
    case 1: pageHours(); break;
    case 2: pageDays(); break;
    case 3: pageSun(); break;
    case 4: pageAir(); break;
    case 5: pageSensors(); break;
  }
  drawFooter();
  needRedraw = false;
}

// Apaga so a area de conteudo (sem piscar cabecalho) - usado na pagina de sensores
void refreshSensorsPage() {
  tft.fillRect(0, 26, 320, 200, C_BG);
  pageSensors();
}

// ============================== OLED SH1107 ================================
void oledIcon(int cx, int cy, int code, bool day) {
  if (code <= 1) {
    if (day) { oled.drawDisc(cx, cy, 6); for (int i = 0; i < 8; i++) { float a = i * PI / 4; oled.drawLine(cx + cos(a) * 8, cy + sin(a) * 8, cx + cos(a) * 11, cy + sin(a) * 11); } }
    else { oled.drawDisc(cx, cy, 7); oled.setDrawColor(0); oled.drawDisc(cx + 4, cy - 3, 7); oled.setDrawColor(1); }
  } else if (code == 45 || code == 48) {
    for (int i = -1; i <= 1; i++) oled.drawBox(cx - 11, cy + i * 5, 22, 2);
  } else {
    oled.drawDisc(cx - 6, cy, 5); oled.drawDisc(cx + 6, cy, 5); oled.drawDisc(cx, cy - 4, 7);
    oled.drawBox(cx - 6, cy, 12, 6);
    if (code >= 51 && code < 95) for (int i = -1; i <= 1; i++) oled.drawLine(cx + i * 6, cy + 8, cx + i * 6 - 3, cy + 13);
    if (code >= 95) { oled.setDrawColor(0); oled.drawTriangle(cx, cy - 2, cx - 4, cy + 5, cx + 2, cy + 5); oled.setDrawColor(1);
                      oled.drawTriangle(cx - 1, cy + 6, cx + 4, cy + 6, cx - 3, cy + 13); }
  }
}

void drawOled() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 10, "ALAGOINHAS");
  oled.drawStr(98, 10, wifiOk() ? "WiFi" : "OFF");
  oled.drawHLine(0, 13, 128);

  oled.setFont(u8g2_font_logisoso20_tn);
  String hm = hhmm();
  oled.drawStr((128 - oled.getStrWidth(hm.c_str())) / 2, 39, hm.c_str());

  if (wx.ok) {
    char t[12]; snprintf(t, sizeof(t), "%d\xb0" "C", (int)round(wx.temp));
    oled.setFont(u8g2_font_logisoso24_tf);
    oled.drawStr(2, 72, t);                 // \xb0 = simbolo de grau (Latin-1)
    oledIcon(106, 58, wx.code, wx.isDay);
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(2, 86, weatherText(wx.code));
  } else {
    oled.setFont(u8g2_font_6x12_tf);
    oled.drawStr(2, 66, "Buscando clima...");
  }
  oled.drawHLine(0, 90, 128);

  oled.setFont(u8g2_font_6x12_tf);
  char l[24];
  if (loc.dhtOk) snprintf(l, sizeof(l), "Casa %.1f\xb0  %d%%", loc.t, (int)round(loc.h));
  else           snprintf(l, sizeof(l), "Casa: sensor erro");
  oled.drawStr(2, 103, l);

  // linha de status: alerta invertido (fundo aceso) ou "Tudo OK"
  if (alertLevel > 0 && nAlerts > 0) {
    oled.drawBox(0, 108, 128, 20);
    oled.setDrawColor(0);
    int idx = (millis() / 3000) % nAlerts;    // alterna entre os alertas a cada 3 s
    // fonte 5x8: cabem 25 letras por linha; quebra a mensagem em ate 2 linhas
    oled.setFont(u8g2_font_5x8_tf);
    const char* msg = alerts[idx].msg;
    int len = strlen(msg), cut = len;
    if (len > 25) { cut = 25; while (cut > 0 && msg[cut] != ' ') cut--; if (cut == 0) cut = 25; }
    char l1[26], l2[26];
    strlcpy(l1, msg, min(cut + 1, 26));
    strlcpy(l2, msg + cut + (msg[cut] == ' ' ? 1 : 0), sizeof(l2));
    oled.drawStr(2, 117, l1);
    oled.drawStr(2, 126, l2);
    oled.setDrawColor(1);
  } else {
    oled.drawStr(2, 122, "Sem alertas :)");
  }
  oled.sendBuffer();
}

// ================================ BOTOES ===================================
bool pressed(int pin, bool& last, unsigned long& t) {
  bool now = digitalRead(pin) == LOW;          // pull-up: apertado = LOW
  bool edge = now && !last && (millis() - t > 200);
  if (edge) t = millis();
  last = now;
  return edge;
}

void handleButtons() {
  static bool lastN = false, lastP = false;
  static unsigned long tN = 0, tP = 0;
  if (pressed(BTN_NEXT, lastN, tN)) { page = (page + 1) % NUM_PAGES; }
  else if (pressed(BTN_PREV, lastP, tP)) { page = (page + NUM_PAGES - 1) % NUM_PAGES; }
  else return;
  lastManual = lastPageChange = millis();
  needRedraw = true;
  Serial.printf("[BOTAO] pagina %d: %s\n", page, PAGE_TITLE[page]);
}

// ============================== WI-FI / NTP ================================
void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  Serial.print("[WiFi] conectando");
  unsigned long t0 = millis();
  while (!wifiOk() && millis() - t0 < 15000) { delay(250); Serial.print("."); }
  Serial.println();
  if (wifiOk()) {
    Serial.printf("[WiFi] conectado! IP %s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, 0, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println("[WiFi] falhou - vou tentar de novo em segundo plano");
  }
}

void splash(const char* status) {
  tft.fillScreen(C_BG);
  txtC(160, 70, "NEXORA", 4, C_ACCENT);
  txtC(160, 110, "Estacao Meteorologica", 2, C_TEXT);
  txtC(160, 135, CITY_NAME, 1, C_MUTED);
  txtC(160, 180, status, 1, C_YELLOW);
}

// ================================= SETUP ===================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== NEXORA - Estacao Meteorologica ===");

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLed(0, 0, 1);

  tft.begin();
  tft.setRotation(1);                       // paisagem: 320 x 240
  splash("Iniciando sensores...");

  Wire.begin(I2C_SDA, I2C_SCL);
  oled.begin();
  oled.clearBuffer(); oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(20, 60, "NEXORA..."); oled.sendBuffer();

  dht.begin();
  loc.bmpOk = bmp.begin();
  Serial.printf("[SENSOR] BMP180 %s\n", loc.bmpOk ? "encontrado" : "NAO encontrado");

  splash("Conectando ao Wi-Fi...");
  startWifi();
  splash(wifiOk() ? "Wi-Fi OK! Buscando o tempo..." : "Sem Wi-Fi - tentando de novo");

  delay(1500);                              // espera o primeiro sinal do NTP/DHT
  readSensors();
  evaluateAlerts();
  lastPageChange = millis();
}

// ================================= LOOP ====================================
void loop() {
  unsigned long now = millis();
  handleButtons();

  // Reconexao do Wi-Fi sem travar o painel
  if (!wifiOk() && now - lastWifiTry > 30000) {
    lastWifiTry = now;
    Serial.println("[WiFi] reconectando...");
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  }

  // Previsao do tempo
  if (wifiOk()) {
    bool due = firstWeather ? (now - lastWeatherTry > RETRY_EVERY || lastWeatherTry == 0)
                            : (now - lastWeather >= WEATHER_EVERY);
    if (!firstWeather && !wx.ok) due = now - lastWeatherTry > RETRY_EVERY;
    if (due) {
      lastWeatherTry = now;
      if (fetchWeather()) { lastWeather = millis(); firstWeather = false; }
      evaluateAlerts(); needRedraw = true;
    }
    bool dueAir = firstAir ? (now - lastAirTry > RETRY_EVERY || lastAirTry == 0)
                           : (now - lastAir >= AIR_EVERY);
    if (dueAir) {
      lastAirTry = now;
      if (fetchAir()) { lastAir = millis(); firstAir = false; }
      evaluateAlerts(); if (page == 4) needRedraw = true;
    }
  }

  // Sensores locais
  if (now - lastSensor >= SENSOR_EVERY) {
    lastSensor = now;
    readSensors();
    evaluateAlerts();
    if (page == 5 && !needRedraw) refreshSensorsPage();
  }

  // Troca automatica de pagina (pausada por 30 s depois de usar um botao)
  if (now - lastPageChange >= PAGE_EVERY && now - lastManual >= MANUAL_HOLD) {
    page = (page + 1) % NUM_PAGES;
    lastPageChange = now;
    needRedraw = true;
  }

  if (needRedraw) drawPage();

  // A cada segundo: relogio, OLED e LED
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    blinkState = !blinkState;
    drawClock();
    drawOled();
    updateLed();
  }
  delay(10);
}
