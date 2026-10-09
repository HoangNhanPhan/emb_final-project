# MQTT Handler for ESP32

Non-blocking MQTT client with automatic WiFi/MQTT reconnection.

## Features
- Automatic WiFi reconnection
- Automatic MQTT reconnection with exponential backoff
- Non-blocking operation (call update() periodically)
- Telemetry publishing (SensorData_t -> JSON)
- Command subscription (JSON -> ControlCommand_t)
- ArduinoJson integration
- Status monitoring

## Usage
```cpp
#include "MQTTHandler.h"

MQTTHandler mqtt;
mqtt.begin();

// In main loop or task:
mqtt.update();

// Publish telemetry:
if (mqtt.isConnected()) {
    SensorData_t data = getSensorData();
  mqtt.publishTelemetry(data, fanDutyPct, fanRpm, targetTemperature,
              mode, alarm, shutdown);
}

// Check for commands:
if (mqtt.hasCommand()) {
    ControlCommand_t cmd = mqtt.getCommand();
    // Process the Remote setpoint or Operator action.
}
```

## Topics
- Telemetry: `env/telemetry` (publish)
- Commands: `env/command` (subscribe)  
- Alerts: `env/alert` (publish)

## Message Formats
**Telemetry (published):**
```json
{
  "device_id": "esp32-fan-controller",
  "timestamp": 1234567,
  "temperature": 25.5,
  "humidity": 60.2,
  "fan_duty": 75,
  "fan_rpm": 1800,
  "mode": "REMOTE",
  "alarm": false,
  "shutdown": false,
  "target_temperature": 30.0,
  "sensor_valid": true
}
```

**Commands (subscribed):**
```json
{"duty": 75}
```

`duty` sets the Remote fan duty. The physical Activate button on GPIO33 enters Operator mode from Auto or Remote; the App action `{"action":"activate"}` is also supported. In Operator mode, one momentary button on GPIO32 toggles the output ON/OFF on each press. Connect each button between its GPIO and GND. GPIO34 reads the 12-bit potentiometer and maps ADC 0-4095 directly to PWM 0-1023. ADC changes below 16 counts are ignored; 30 seconds without a qualifying adjustment returns the system to Auto. MQTT `on`/`off` actions remain supported too.