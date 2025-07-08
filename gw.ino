#include <MySensors.h>
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <TimeLib.h> // For time conversion

#define SerialMon Serial
#define SerialAT Serial1

// GSM/REST setup
const char apn[]      = "your_apn";
const char gprsUser[] = "";
const char gprsPass[] = "";
const char ha_server[] = "your_homeassistant_ip_or_domain";
const int ha_port = 8123;
const char ha_token[] = "YOUR_LONG_LIVED_ACCESS_TOKEN";

const String ha_entity = "sensor.mysensor_gateway";
const String ha_state_endpoint = "/api/states/" + ha_entity;
const char* ha_cmd_endpoint = "/api/webhook/gw_commands";
const char* ha_cmd_update_endpoint = "/api/webhook/gw_commands_update";

#define MAX_NODES 8
#define ENERGY_INTERVAL  5000UL      // 5 seconds for energy sensor reading
#define REPORT_INTERVAL  3600000UL   // 1 hour

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
HttpClient http(client, ha_server, ha_port);

struct NodeData {
  uint8_t nodeId = 0;
  float pulse = 0, temp = 0, hum = 0, batt = 0;
  unsigned long lastUpdate = 0;
};
NodeData nodes[MAX_NODES];

float gwEnergy = 0;
unsigned long lastReport = 0;
unsigned long lastEnergyRead = 0;
unsigned long stayOnUntil = 0;

// ========== MySensors Callbacks =============
void presentation() {}

void receive(const MyMessage &message) {
  uint8_t nodeId = message.sender;
  uint8_t type   = message.type;
  float value    = message.getFloat();

  NodeData* nd = nullptr;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].nodeId == nodeId || nodes[i].nodeId == 0) {
      nd = &nodes[i];
      nd->nodeId = nodeId;
      break;
    }
  }
  if (!nd) return;

  switch (type) {
    case V_VAR1:    nd->pulse = value; break;
    case V_TEMP:    nd->temp  = value; break;
    case V_HUM:     nd->hum   = value; break;
    case V_VOLTAGE: nd->batt  = value; break;
  }
  nd->lastUpdate = millis();
}

// ========== Dummy Energy Sensor ============
float readEnergySensor() {
  gwEnergy += random(-2, 3) * 0.03;
  if (gwEnergy < 0) gwEnergy = 0;
  return gwEnergy;
}

// ========== Send time to all nodes =========
void sendTimeToNodes(time_t unixTime) {
  MyMessage timeMsg(0, V_TIME);
  timeMsg.set((uint32_t)unixTime);
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].nodeId > 0) {
      timeMsg.sensor = nodes[i].nodeId;
      send(timeMsg);
      SerialMon.print("Sent time to node "); SerialMon.println(nodes[i].nodeId);
    }
  }
}

// ========== Push data to HA =========
void pushToHA() {
  StaticJsonDocument<768> doc;
  doc["state"] = gwEnergy;
  JsonObject attr = doc.createNestedObject("attributes");
  attr["timestamp"] = millis();
  attr["gw_energy"] = gwEnergy;
  JsonArray nodesArray = attr.createNestedArray("nodes");
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].nodeId > 0) {
      JsonObject n = nodesArray.createNestedObject();
      n["node_id"] = nodes[i].nodeId;
      n["pulse"]   = nodes[i].pulse;
      n["temp"]    = nodes[i].temp;
      n["hum"]     = nodes[i].hum;
      n["batt"]    = nodes[i].batt;
      n["last"]    = nodes[i].lastUpdate;
    }
  }
  String payload;
  serializeJson(doc, payload);

  http.beginRequest();
  http.put(ha_state_endpoint.c_str());
  http.sendHeader("Authorization", String("Bearer ") + ha_token);
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", payload.length());
  http.beginBody();
  http.print(payload);
  http.endRequest();
  http.responseStatusCode();
  http.responseBody();
}

// ========== Poll and Update Commands =========
void pollAndUpdateCommands() {
  // 1. GET Command List
  http.get(ha_cmd_endpoint);
  int statusCode = http.responseStatusCode();
  String response = http.responseBody();
  if (statusCode != 200 || response.length() == 0) return;

  // 2. Parse and process commands
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return;

  bool stayOnCmdFound = false;
  bool anyCommandProcessed = false;

  for (JsonObject cmd : doc.as<JsonArray>()) {
    if (cmd.containsKey("result")) continue; // Already processed

    // Only process if not read yet
    if (!cmd.containsKey("read")) {
      // Mark as read
      String nowStr = String(year()) + "-";
      if (month() < 10) nowStr += "0";
      nowStr += String(month()) + "-";
      if (day() < 10) nowStr += "0";
      nowStr += String(day()) + "T";
      if (hour() < 10) nowStr += "0";
      nowStr += String(hour()) + ":";
      if (minute() < 10) nowStr += "0";
      nowStr += String(minute()) + ":";
      if (second() < 10) nowStr += "0";
      nowStr += String(second()) + "Z";
      cmd["read"] = nowStr;

      String command = cmd["cmd"] | "";
      JsonObject args = cmd["args"];
      String result = "OK";

      if (command == "reset_counters") {
        if (args.containsKey("nodes")) {
          for (JsonVariant v : args["nodes"].as<JsonArray>()) {
            uint8_t nodeId = v.as<uint8_t>();
            for (int i = 0; i < MAX_NODES; i++) {
              if (nodes[i].nodeId == nodeId) nodes[i].pulse = 0;
            }
          }
        } else {
          for (int i = 0; i < MAX_NODES; i++)
            if (nodes[i].nodeId > 0) nodes[i].pulse = 0;
        }
      } else if (command == "reset_energy") {
        gwEnergy = 0;
      } else if (command == "set_time") {
        if (args.containsKey("time")) {
          String timestr = args["time"].as<String>();
          int year_, month_, day_, hour_, minute_, second_;
          if (sscanf(timestr.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year_, &month_, &day_, &hour_, &minute_, &second_) == 6) {
            setTime(hour_, minute_, second_, day_, month_, year_);
            time_t unixTime = now();
            sendTimeToNodes(unixTime);
          } else {
            result = "Error: Invalid time format";
          }
        }
      } else if (command == "stay_on") {
        if (args.containsKey("minutes")) {
          uint32_t minutes = args["minutes"];
          stayOnUntil = millis() + (minutes * 60000UL);
          stayOnCmdFound = true;
        }
      } else if (command == "finish") {
        // handled after loop
      } else {
        result = "Error: Unknown command";
      }

      cmd["result"] = result;
      anyCommandProcessed = true;
    }
  }

  // 3. PUT updated command list back to HA
  if (anyCommandProcessed) {
    String updatedPayload;
    serializeJson(doc, updatedPayload);
    http.beginRequest();
    http.put(ha_cmd_update_endpoint);
    http.sendHeader("Authorization", String("Bearer ") + ha_token);
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", updatedPayload.length());
    http.beginBody();
    http.print(updatedPayload);
    http.endRequest();
    http.responseStatusCode();
    http.responseBody();
  }

  // 4. Power down if finish and not stay_on
  bool finish = false;
  for (JsonObject cmd : doc.as<JsonArray>()) {
    if (cmd["cmd"] == "finish" && cmd.containsKey("result") && String(cmd["result"]) == "OK")
      finish = true;
  }
  if (finish && !stayOnCmdFound) {
    modem.gprsDisconnect();
    modem.poweroff();
    stayOnUntil = 0;
  }
}

// ========== Setup ===========
void setup() {
  SerialMon.begin(115200);
  SerialAT.begin(115200);
  delay(3000);

  modem.restart();
  modem.gprsConnect(apn, gprsUser, gprsPass);
  lastReport = millis();
  lastEnergyRead = millis();
}

// ========== Loop ===========
void loop() {
  unsigned long now = millis();

  if (now - lastEnergyRead >= ENERGY_INTERVAL) {
    lastEnergyRead = now;
    readEnergySensor();
  }

  if ((now - lastReport >= REPORT_INTERVAL) || (stayOnUntil > 0 && now < stayOnUntil)) {
    lastReport = now;

    if (!modem.isGprsConnected())
      modem.gprsConnect(apn, gprsUser, gprsPass);

    pushToHA();
    pollAndUpdateCommands();

    if (stayOnUntil > 0 && now < stayOnUntil) delay(60000);
  }
}
