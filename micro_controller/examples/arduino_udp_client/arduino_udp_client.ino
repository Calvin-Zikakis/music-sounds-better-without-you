// Arduino UDP Pub/Sub Client Example
// Compatible with ESP32. For ESP8266, change WiFi.h to ESP8266WiFi.h.
// For other Arduinos, you'll need an appropriate network shield and library.

#include <WiFi.h>        // For ESP32 Wi-Fi
#include <WiFiUdp.h>     // For UDP communication

// Wi-Fi Credentials
const char* ssid = "YOUR_WIFI_SSID";         // Replace with your Wi-Fi SSID
const char* password = "YOUR_WIFI_PASSWORD"; // Replace with your Wi-Fi Password

// Pub/Sub Server Configuration
const char* udpServerIp = "127.0.0.1"; // IP address of your Rust UDP server
                                       // If Arduino is on a different machine than the server,
                                       // use the server's actual local network IP (e.g., 192.168.1.X)
const uint16_t udpServerPort = 7878;   // Port of your Rust UDP server (matches BIND_ADDRESS)
const uint16_t localUdpPort = 4210;    // Local port for this Arduino to listen on (can be any free port)

WiFiUDP udp; // UDP object

// Buffer for incoming UDP packets
char packetBuffer[255];
// Buffer for outgoing messages
char messageBuffer[255];

unsigned long lastPublishTime = 0;
const unsigned long publishInterval = 10000; // Publish every 10 seconds

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting Arduino UDP Pub/Sub Client...");

  connectToWiFi();

  Serial.println("Initializing UDP...");
  if (udp.begin(localUdpPort)) {
    Serial.print("UDP Listening on port: ");
    Serial.println(localUdpPort);
  } else {
    Serial.println("Failed to initialize UDP!");
    // Handle error - perhaps restart or enter a safe mode
    while (true);
  }

  // Subscribe to a channel on startup
  subscribeToChannel("arduino_commands");
  subscribeToChannel("general_alerts");
}

void loop() {
  // Check for incoming UDP packets
  int packetSize = udp.parsePacket();
  if (packetSize) {
    Serial.print("Received packet of size ");
    Serial.print(packetSize);
    Serial.print(" from ");
    Serial.print(udp.remoteIP());
    Serial.print(":");
    Serial.println(udp.remotePort());

    int len = udp.read(packetBuffer, 254); // Read packet into buffer
    if (len > 0) {
      packetBuffer[len] = 0; // Null-terminate the string
    }
    Serial.print("UDP Packet Contents: ");
    Serial.println(packetBuffer);

    // TODO: Add logic here to parse and act on received messages
    // For example, if message is "LED:ON", turn an LED on.
    // Messages received here are payloads from channels this Arduino is subscribed to.
    // The format is just the raw payload, e.g., "temperature=25.3"
    // or "ALERT:System Overheat"
  }

  // Periodically publish a message
  if (millis() - lastPublishTime > publishInterval) {
    publishMessage("sensor_data", "temperature=23.5;humidity=45.2");
    lastPublishTime = millis();
  }

  // Add other logic here, e.g., reading sensors and publishing their data
}

void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi SSID: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("Wi-Fi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Helper function to send a command to the UDP server
void sendUdpCommand(const char* command) {
  udp.beginPacket(udpServerIp, udpServerPort);
  udp.print(command);
  if (udp.endPacket()) {
    Serial.print("Sent command: ");
    Serial.println(command);
  } else {
    Serial.print("Failed to send command: ");
    Serial.println(command);
  }
}

// Subscribe to a channel
void subscribeToChannel(const char* channelName) {
  snprintf(messageBuffer, sizeof(messageBuffer), "SUB:%s", channelName);
  sendUdpCommand(messageBuffer);
}

// Unsubscribe from a channel
void unsubscribeFromChannel(const char* channelName) {
  snprintf(messageBuffer, sizeof(messageBuffer), "UNSUB:%s", channelName);
  sendUdpCommand(messageBuffer);
}

// Publish a message to a channel
void publishMessage(const char* channelName, const char* payload) {
  snprintf(messageBuffer, sizeof(messageBuffer), "PUB:%s:%s", channelName, payload);
  sendUdpCommand(messageBuffer);
}

/*
Example Usage:
1. Replace "YOUR_WIFI_SSID" and "YOUR_WIFI_PASSWORD" with your network details.
2. If your Rust server is not running on the same machine or if 127.0.0.1 is not accessible
   from your Arduino's network, change `udpServerIp` to the actual IP address of the
   machine running the Rust server.
3. Upload this sketch to your ESP32.
4. Open the Serial Monitor.
5. Run the Rust `subpub_server` application.

You should see:
- The Arduino connecting to Wi-Fi.
- UDP initialization messages.
- "SUB:arduino_commands" and "SUB:general_alerts" being sent.
- Periodically, "PUB:sensor_data:temperature=23.5;humidity=45.2" being sent.
- If other clients (or this Arduino itself, if modified) publish to "arduino_commands"
  or "general_alerts", this Arduino will receive and print those messages.

To test receiving:
- Use a separate UDP client (like netcat/nc, or another Arduino/Python script) to publish
  to the "arduino_commands" channel.
  Example using netcat (assuming server is at 127.0.0.1:7878):
  echo -n "PUB:arduino_commands:LED_ON" | nc -u -w1 127.0.0.1 7878
  The Arduino should then print the received "LED_ON" payload.
*/
