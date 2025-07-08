#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <TimeLib.h>

// ========== Configuration ==========
#define SerialMon Serial
#define SerialAT Serial1   // Hardware serial connected to SIM7xxx
#define TINY_GSM_MODEM_SIM7000

const char apn[]      = "your_apn";
const char gprsUser[] = "";
const char gprsPass[] = "";
const char ha_server[] = "your_homeassistant_domain_or_ip"; // Use domain with valid SSL cert!
const int ha_port     = 443;
const char ha_token[] = "YOUR_LONG_LIVED_ACCESS_TOKEN";

const char* ha_post_endpoint = "/api/webhook/gw_serial";
const char* ha_poll_endpoint = "/api/webhook/gw_serial_poll";

#define MAX_NODES 8
#define POWER_INTERVAL  5000UL
#define MINUTE_INTERVAL 60000UL
#define HOUR_INTERVAL  3600000UL

#define POWER_ADC_PIN     A0
#define ADC_REF_VOLTAGE   3.3
#define ADC_RESOLUTION    4096
#define CURRENT_BIAS      1.65
#define CURRENT_SENSITIVITY 50.0
#define LINE_VOLTAGE      230.0
#define RMS_SAMPLES       1000

#ifndef V_VOLUME
#define V_VOLUME 47
#endif
#ifndef V_TEMP
#define V_TEMP 0
#endif
#ifndef V_HUM
#define V_HUM 1
#endif
#ifndef V_VOLTAGE
#define V_VOLTAGE 38
#endif

// ========== Data Structures ==========
struct NodeData {
  uint8_t nodeId = 0;
  float volume = 0, temp = 0, hum = 0, batt = 0;
  unsigned long lastUpdate = 0;
};
NodeData nodes[MAX_NODES];

float gwPower = 0;
float gwEnergy = 0;
unsigned long lastPowerSample = 0;
unsigned long lastMinuteReport = 0;
unsigned long lastHourReport = 0;
unsigned long stayOnUntil = 0;

struct GWCommand {
  String id;
  String cmdline;
  bool read = false;
  bool finished = false;
  String result;
};
#define MAX_CMD_QUEUE 8
GWCommand cmdQueue[MAX_CMD_QUEUE];
uint8_t cmdQueueLen = 0;

// ========== TinyGSM HTTPS Setup ==========
TinyGsm modem(SerialAT);
TinyGsmClientSecure gsmClient(modem); // Secure client for HTTPS
HttpClient http(gsmClient, ha_server, ha_port);

// For SIM7xxx HTTPS, you may need to set root CA or skip validation!
const char ca_cert[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
...Your Root CA or Let's Encrypt CA...
-----END CERTIFICATE-----
)EOF";

// ========== Helper: Robust modem/network startup ==========
void ensureModemAndGPRS() {
  SerialMon.println("Restarting modem...");
  modem.restart();

  SerialMon.print("Waiting for network...");
  unsigned long start = millis();
  while (!modem.waitForNetwork(60000)) {
    SerialMon.println(" No network, retry...");
    delay(5000);
    if (millis() - start > 120000UL) {
      SerialMon.println("Network wait timed out, resetting...");
      modem.restart();
      start = millis();
    }
  }
  SerialMon.println(" Network OK!");

  SerialMon.print("Connecting to GPRS...");
  start = millis();
  while (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println(" GPRS failed, retrying...");
    delay(5000);
    if (millis() - start > 120000UL) {
      SerialMon.println("GPRS wait timed out, resetting...");
      modem.restart();
      modem.waitForNetwork(60000);
      start = millis();
    }
  }
  SerialMon.println(" GPRS connected!");
}

// ========== MySensors Data Formatting ===========
String formatNodeLine(uint8_t nodeId, uint8_t childId, uint8_t msgType, uint8_t ack, uint8_t subType, String payload) {
  String ln = String(nodeId) + ";" + String(childId) + ";" + String(msgType) + ";" + String(ack) + ";" + String(subType) + ";" + payload;
  return ln;
}

// ========== Simulate MySensors Receive ===========
void receiveMySensorsLine(const String& line) {
  int field[5] = {0};
  int idx = 0, last = 0;
  for (int i = 0; i < 5; ++i) {
    idx = line.indexOf(';', last);
    if (idx < 0) return;
    field[i] = line.substring(last, idx).toInt();
    last = idx + 1;
  }
  String payload = line.substring(last);
  uint8_t nodeId = field[0];
  uint8_t childId = field[1];
  uint8_t msgType = field[2];
  uint8_t ack     = field[3];
  uint8_t subType = field[4];

  NodeData* nd = nullptr;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].nodeId == nodeId || nodes[i].nodeId == 0) {
      nd = &nodes[i];
      nd->nodeId = nodeId;
      break;
    }
  }
  if (!nd) return;
  if (msgType == 1) {
    float value = payload.toFloat();
    switch (subType) {
      case V_VOLUME:  nd->volume = value; break;
      case V_TEMP:    nd->temp   = value; break;
      case V_HUM:     nd->hum    = value; break;
      case V_VOLTAGE: nd->batt   = value; break;
    }
    nd->lastUpdate = millis();
  }
}

// ========== Power/Energy Calculation ===========
float readPowerSensor() {
  float sumOfSquares = 0;
  for (uint16_t i = 0; i < RMS_SAMPLES; i++) {
    int adcValue = analogRead(POWER_ADC_PIN);
    float voltage = (adcValue * ADC_REF_VOLTAGE) / (ADC_RESOLUTION - 1);
    float centered = voltage - CURRENT_BIAS;
    sumOfSquares += centered * centered;
    delayMicroseconds(200);
  }
  float v_rms = sqrt(sumOfSquares / RMS_SAMPLES);
  float i_rms = v_rms * CURRENT_SENSITIVITY;
  float powerVA = i_rms * LINE_VOLTAGE;
  gwPower = powerVA;
  return gwPower;
}
void updateEnergy() {
  float intervalHours = POWER_INTERVAL / 3600000.0;
  float increment = gwPower * intervalHours;
  gwEnergy += increment;
}

// ========== Data Push: Compose MySensors lines ===========
void pushAllToHA() {
  String out = "";

  out += formatNodeLine(0, 255, 3, 0, 30, "POWER:" + String(gwPower, 2)) + "\n";
  out += formatNodeLine(0, 255, 3, 0, 31, "ENERGY:" + String(gwEnergy, 6)) + "\n";

  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].nodeId > 0) {
      out += formatNodeLine(nodes[i].nodeId, 1, 1, 0, V_VOLUME, String(nodes[i].volume, 3)) + "\n";
      out += formatNodeLine(nodes[i].nodeId, 2, 1, 0, V_TEMP, String(nodes[i].temp, 2)) + "\n";
      out += formatNodeLine(nodes[i].nodeId, 3, 1, 0, V_HUM, String(nodes[i].hum, 2)) + "\n";
      out += formatNodeLine(nodes[i].nodeId, 4, 1, 0, V_VOLTAGE, String(nodes[i].batt, 3)) + "\n";
    }
  }
  for (uint8_t i = 0; i < cmdQueueLen; i++) {
    if (cmdQueue[i].read && !cmdQueue[i].finished) {
      out += formatNodeLine(0, 255, 3, 0, 40, cmdQueue[i].id + ";READ") + "\n";
    }
    if (cmdQueue[i].finished) {
      out += formatNodeLine(0, 255, 3, 0, 41, cmdQueue[i].id + ";FINISHED;" + cmdQueue[i].result) + "\n";
    }
  }

  http.connectionKeepAlive(); // Try to reuse connection if possible

  http.beginRequest();
  http.post(ha_post_endpoint);
  http.sendHeader("Authorization", String("Bearer ") + ha_token);
  http.sendHeader("Content-Type", "text/plain");
  http.sendHeader("Content-Length", out.length());
  http.beginBody();
  http.print(out);
  http.endRequest();
  http.responseStatusCode();
  http.responseBody();
}

// ========== Poll and Receive Commands ===========
void pollCommandsFromHA() {
  http.connectionKeepAlive();

  http.get(ha_poll_endpoint);
  int statusCode = http.responseStatusCode();
  String response = http.responseBody();
  if (statusCode != 200 || response.length() == 0) return;

  int start = 0;
  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = response.substring(start, end);
    line.trim();
    if (line.length() > 0 && cmdQueueLen < MAX_CMD_QUEUE) {
      GWCommand c;
      c.id = String(cmdQueueLen);
      c.cmdline = line;
      c.read = false;
      c.finished = false;
      c.result = "";
      cmdQueue[cmdQueueLen++] = c;
    }
    start = end + 1;
  }
}

// ========== Process Command Queue =============
void processCommandQueue() {
  for (uint8_t i = 0; i < cmdQueueLen; i++) {
    if (!cmdQueue[i].read) {
      cmdQueue[i].read = true;
      // Here: parse/execute MySensors command in cmdline
      cmdQueue[i].finished = true;
      cmdQueue[i].result = "OK";
    }
  }
}

// ========== Setup ===========
void setup() {
  SerialMon.begin(115200);
  SerialAT.begin(115200);
  delay(3000);

  // For HTTPS: set CA if desired (optional, for strict verification)
  // gsmClient.setCACert(ca_cert);
  gsmClient.setInsecure(); // Accept all certificates (not recommended for production, but helps with self-signed or Let's Encrypt on many modems)

  ensureModemAndGPRS();

  lastPowerSample = millis();
  lastMinuteReport = millis();
  lastHourReport = millis();
}

// ========== Loop ===========
void loop() {
  unsigned long now = millis();

  if (now - lastPowerSample >= POWER_INTERVAL) {
    lastPowerSample = now;
    readPowerSensor();
    updateEnergy();
  }

  if ((stayOnUntil > 0 && now < stayOnUntil) && (now - lastMinuteReport >= MINUTE_INTERVAL)) {
    lastMinuteReport = now;
    if (!modem.isGprsConnected()) ensureModemAndGPRS();
    pushAllToHA();
    pollCommandsFromHA();
    processCommandQueue();
  }

  if ((stayOnUntil == 0) && (now - lastHourReport >= HOUR_INTERVAL)) {
    lastHourReport = now;
    if (!modem.isGprsConnected()) ensureModemAndGPRS();
    pushAllToHA();
    pollCommandsFromHA();
    processCommandQueue();
  }
}
