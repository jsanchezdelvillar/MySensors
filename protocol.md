# Gateway ↔ Home Assistant Communication Protocol

This document describes the **custom protocol** used for communication between the Gateway (GW) and Home Assistant (HA), as implemented in the provided code.  
It is a simple line-based text protocol, using semicolon-delimited fields, inspired by (but not fully compliant with) MySensors.

---

## 1. Message Format

Each message is a single line of text with the following structure:

```
node_id;child_id;msg_type;ack;sub_type;payload
```

- **Fields** are separated by semicolons (`;`)
- **Lines** are separated by newline (`\n`)
- Multiple messages can be sent in a single HTTP request body (one per line)

---

## 2. Direction of Communication

### Gateway → Home Assistant

- Periodically (or on event), the gateway sends a POST request with a body containing one or more lines formatted as above.
- Each line represents a sensor reading, state, or command acknowledgement.

### Home Assistant → Gateway

- When polled, HA responds with a plaintext body containing one or more lines in the same format.
- Each line is a command or instruction for the gateway.

---

## 3. Field Descriptions

| Field      | Description                                        | Example     |
|------------|----------------------------------------------------|-------------|
| node_id    | Node identifier (0 for gateway)                    | `0`         |
| child_id   | Sensor/channel or 255 for internal/command         | `255`       |
| msg_type   | 1=Set, 3=Internal                                  | `1` or `3`  |
| ack        | 0 (unused)                                         | `0`         |
| sub_type   | Custom: see below                                  | `30`, `31`, `40`, `41` |
| payload    | Value or command (see below)                       | Varies      |

---

## 4. Sub-types and Payloads

### Sensor Data (Gateway → HA)

| sub_type | Purpose      | Payload Format              | Example                       |
|----------|-------------|-----------------------------|-------------------------------|
| 30       | Power       | `POWER:<float>`             | `POWER:123.45`                |
| 31       | Energy      | `ENERGY:<float>`            | `ENERGY:12.3456`              |
| 47       | Volume      | `<float>`                   | `123.4`                       |
| 0        | Temperature | `<float>`                   | `22.5`                        |
| 1        | Humidity    | `<float>`                   | `77.2`                        |
| 38       | Voltage     | `<float>`                   | `3.85`                        |

### Command Handling (HA ↔ Gateway)

#### HA → Gateway: Command

- **sub_type:** `31`
- **payload:** `cmd_id;command;param1;param2;...`
- **Example:**  
  `0;255;3;0;31;cmd_001;set_time;2025-07-08T23:00:00Z`

#### Gateway → HA: Command Read Acknowledgement

- **sub_type:** `40`
- **payload:** `cmd_id;READ`
- **Example:**  
  `0;255;3;0;40;cmd_001;READ`

#### Gateway → HA: Command Finished

- **sub_type:** `41`
- **payload:** `cmd_id;FINISHED;<result>`
- **Example:**  
  `0;255;3;0;41;cmd_001;FINISHED;OK`

---

## 5. HTTP Endpoints

- **Gateway → HA:**  
  - POST `/api/webhook/gw_serial`  
    Body: plain text, one or more protocol lines (see above)

- **HA → Gateway:**  
  - GET `/api/webhook/gw_serial_poll`  
    Response: plain text, one or more protocol lines (see above)

---

## 6. Example Message Exchange

**Gateway → HA (POST body):**
```
0;255;3;0;30;POWER:123.45
0;255;3;0;31;ENERGY:12.3456
1;1;1;0;47;120.0
```

**HA → Gateway (poll response):**
```
0;255;3;0;31;cmd_002;set_time;2025-07-08T23:00:00Z
```

**Gateway → HA (acknowledgements):**
```
0;255;3;0;40;cmd_002;READ
0;255;3;0;41;cmd_002;FINISHED;OK
```

---

## 7. List of Supported Commands (HA → GW)

**All commands are sent using:**
```
0;255;3;0;31;cmd_id;command;[param1];[param2];...
```
Where:
- `cmd_id` = Unique command identifier for tracking
- `command` = Command name (see below)
- `paramN` = Additional parameters as required by the command

### Supported Commands

| Command Name   | Parameters                                  | Description                                                        | Example Payload                                    |
|----------------|---------------------------------------------|--------------------------------------------------------------------|----------------------------------------------------|
| set_time       | `<iso8601_time>`                            | Set the gateway clock to the specified UTC time                    | `cmd_001;set_time;2025-07-08T23:00:00Z`            |
| calibrate      | `<sensor_id>` `<cal_value>`                 | Calibrate the specified sensor with value                          | `cmd_002;calibrate;1;1000.0`                       |
| stay_on        | `<duration_seconds>`                        | Keep the gateway in active/polling mode for the specified duration | `cmd_003;stay_on;600`                              |
| reset_energy   | (none)                                      | Reset the cumulative energy counter                                | `cmd_004;reset_energy`                             |
| set_config     | `<key>` `<value>`                           | Set a configuration parameter                                      | `cmd_005;set_config;sampling_interval;10`           |
| reboot         | (none)                                      | Reboot the gateway                                                 | `cmd_006;reboot`                                   |
| custom_command | `<arbitrary>`                               | Reserved for custom/experimental commands                          | `cmd_007;custom_command;some_value`                |

> **Note:**  
> - Parameters are separated by semicolons.  
> - The command list can be expanded as new features are added.

---

## 8. Notes

- The protocol does not implement full MySensors support—only the fields and sub-types above are used.
- The command mechanism allows HA to send arbitrary string commands (with parameters) to GW, and GW to acknowledge and report result.
- All communication is over HTTPS with bearer token authentication.
- The protocol is extensible: you can add new commands, sub_types, or payload formats as needed.

---
