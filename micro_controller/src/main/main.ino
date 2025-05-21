// Arduino UDP Pub/Sub Client Example
// Compatible with ESP32.

#include <WiFi.h>
#include <WiFiUdp.h>

// --- Configuration ---
// Wi-Fi Credentials
const char* WIFI_SSID = "Little Oat's Bark Emporium"; // Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "allonewordlowercase";   // Replace with your Wi-Fi Password

// Sub/Pub Server (will be updated by discovery)
IPAddress udpServerIp;
int udpServerPort = 0; // Set to 0 initially, indicates not yet discovered
const int LOCAL_UDP_PORT = 7877;

// Multicast Discovery (Matches Rust Server)
const char* MULTICAST_ADDRESS_STR = "239.0.0.100";
const int MULTICAST_PORT = 50100;
const char* DISCOVERY_MESSAGE = "DISCOVER_SUBPUB_SERVER";
const char* DISCOVERY_RESPONSE_PREFIX = "SUBPUB_SERVER_AT: ";

// Timings
const unsigned long PUBLISH_INTERVAL_MS = 5000;       // Publish every 5 seconds
const unsigned long DISCOVERY_ATTEMPT_INTERVAL_MS = 30000; // Try discovery every 30 seconds if not found
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000; // Try to reconnect WiFi every 10 seconds if lost
const unsigned long DISCOVERY_TIMEOUT_MS = 5000;      // Timeout for waiting for a discovery response

// --- Global Variables ---
WiFiUDP udp;
char packetBuffer[255];
char messageBuffer[255];

unsigned long lastPublishTime = 0;
unsigned long lastDiscoveryAttemptTime = 0;
unsigned long lastWiFiReconnectAttemptTime = 0;

bool wifiConnected = false;
bool serverDiscovered = false;

// --- Setup ---
void setup() {
  delay(2000); // Allow board to stabilize
  Serial.begin(115200);
  Serial.println("\nStarting Arduino UDP Pub/Sub Client with Discovery...");

  attemptWiFiConnection();
  if (wifiConnected) {
    initializeUdp();
    attemptServerDiscovery();
    if (serverDiscovered) {
      subscribeToInitialChannels();
    }
  }
}

// --- Main Loop ---
void loop() {
  if (!wifiConnected) {
    handleWiFiDisconnected();
    delay(100); // Brief pause before next check
    return;
  }

  if (!serverDiscovered) {
    handleServerNotDiscovered();
  } else {
    // WiFi is connected and server is discovered, proceed with normal operations
    handleIncomingUdpPackets();
    periodicallyPublishData();
  }
  delay(10); // Small delay to keep things responsive
}

// --- WiFi Management ---
void attemptWiFiConnection() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 20; // Approx 10 seconds
  while (WiFi.status() != WL_CONNECTED && tries > 0) {
    Serial.print(".");
    delay(500);
    tries--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
  } else {
    Serial.println("\n[WiFi] Failed to connect.");
    wifiConnected = false;
  }
}

void handleWiFiDisconnected() {
  if (millis() - lastWiFiReconnectAttemptTime > WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("[WiFi] Connection lost. Attempting to reconnect...");
    WiFi.disconnect(true); // Ensure clean disconnect before retrying
    delay(100); // Short delay
    attemptWiFiConnection();
    lastWiFiReconnectAttemptTime = millis();
    if (wifiConnected) { // If reconnected, reset discovery
        serverDiscovered = false;
        initializeUdp(); // Re-initialize UDP as it might have been stopped
        attemptServerDiscovery(); // And try to find the server again
         if (serverDiscovered) {
            subscribeToInitialChannels();
        }
    }
  }
}

// --- UDP & Server Discovery ---
void initializeUdp() {
  Serial.println("[UDP] Initializing...");
  if (udp.begin(LOCAL_UDP_PORT)) {
    Serial.print("[UDP] Listening on port: ");
    Serial.println(LOCAL_UDP_PORT);
  } else {
    Serial.println("[UDP] Failed to initialize!");
    // This is a critical failure for receiving messages.
  }
}

void attemptServerDiscovery() {
  Serial.println("[Discovery] Attempting to discover SubPub server...");
  
  IPAddress multicastIP;
  if (!multicastIP.fromString(MULTICAST_ADDRESS_STR)) {
    Serial.println("[Discovery] ERROR: Failed to parse multicast IP string.");
    serverDiscovered = false;
    return;
  }

  udp.stop(); // Stop current UDP to switch to multicast listening

  if (!udp.beginMulticast(multicastIP, MULTICAST_PORT)) {
    Serial.println("[Discovery] ERROR: Failed to begin multicast listening.");
    udp.begin(LOCAL_UDP_PORT); // Fallback to unicast listening
    serverDiscovered = false;
    return;
  }
  Serial.print("[Discovery] Listening for multicast on ");
  Serial.print(multicastIP);
  Serial.print(":");
  Serial.println(MULTICAST_PORT);

  // Send discovery ping
  udp.beginPacket(multicastIP, MULTICAST_PORT);
  udp.print(DISCOVERY_MESSAGE);
  if (udp.endPacket()) {
    Serial.println("[Discovery] Ping sent to multicast group.");
  } else {
    Serial.println("[Discovery] ERROR: Failed to send ping.");
    udp.stop();
    udp.begin(LOCAL_UDP_PORT); // Fallback
    serverDiscovered = false;
    return;
  }

  // Listen for response
  unsigned long discoveryStartTime = millis();
  bool foundInThisAttempt = false;
  while (millis() - discoveryStartTime < DISCOVERY_TIMEOUT_MS) {
    int packetSize = udp.parsePacket();
    if (packetSize) {
      int len = (packetSize < 254) ? packetSize : 254;
      udp.read(packetBuffer, len);
      packetBuffer[len] = 0;
      Serial.print("[Discovery] Response candidate: ");
      Serial.println(packetBuffer);

      if (strncmp(packetBuffer, DISCOVERY_RESPONSE_PREFIX, strlen(DISCOVERY_RESPONSE_PREFIX)) == 0) {
        char* serverInfo = packetBuffer + strlen(DISCOVERY_RESPONSE_PREFIX);
        char* ipStr = strtok(serverInfo, ":");
        char* portStr = strtok(NULL, ":");

        if (ipStr && portStr && udpServerIp.fromString(ipStr)) {
          int parsedPort = atoi(portStr);
          if (parsedPort > 0 && parsedPort < 65536) {
            udpServerPort = parsedPort;
            Serial.print("[Discovery] Server found at IP: ");
            Serial.print(udpServerIp);
            Serial.print(", Port: ");
            Serial.println(udpServerPort);
            foundInThisAttempt = true;
            serverDiscovered = true; // Set global flag
            break; 
          }
        }
      }
    }
    delay(10);
  }

  udp.stop(); // Stop multicast listening
  if (!udp.begin(LOCAL_UDP_PORT)) { // Re-initialize for unicast listening
      Serial.println("[UDP] CRITICAL: Failed to re-initialize for unicast after discovery!");
      serverDiscovered = false; // Can't operate if UDP is broken
  } else {
      Serial.print("[UDP] Re-initialized for unicast listening on port: ");
      Serial.println(LOCAL_UDP_PORT);
  }
  
  if (!foundInThisAttempt) {
    Serial.println("[Discovery] Server not found in this attempt.");
    serverDiscovered = false; // Ensure it's false if not found
  }
  lastDiscoveryAttemptTime = millis();
}

void handleServerNotDiscovered() {
  if (millis() - lastDiscoveryAttemptTime > DISCOVERY_ATTEMPT_INTERVAL_MS) {
    Serial.println("[State] Server not discovered. Retrying discovery...");
    attemptServerDiscovery();
    if (serverDiscovered) {
        subscribeToInitialChannels();
    }
  }
}

// --- Pub/Sub Operations ---
void subscribeToInitialChannels() {
  Serial.println("[PubSub] Subscribing to initial channels...");
  subscribeToChannel("arduino_commands");
  subscribeToChannel("general_alerts");
  // To receive its own PUB messages, the Arduino must subscribe to the channels it publishes to.
  subscribeToChannel("sensor_data"); 
}

void sendUdpCommand(const char* command) {
  if (!wifiConnected || !serverDiscovered) {
    Serial.print("[PubSub] Cannot send command (WiFi or Server not ready): ");
    Serial.println(command);
    return;
  }

  if (udp.beginPacket(udpServerIp, udpServerPort)) {
    udp.print(command);
    if (udp.endPacket()) {
      Serial.print("[PubSub] Sent to ");
      Serial.print(udpServerIp); Serial.print(":"); Serial.print(udpServerPort);
      Serial.print(" -> "); Serial.println(command);
    } else {
      Serial.println("[PubSub] ERROR: Command send failed (endPacket).");
    }
  } else {
    Serial.println("[PubSub] ERROR: Command send failed (beginPacket).");
  }
}

void subscribeToChannel(const char* channelName) {
  snprintf(messageBuffer, sizeof(messageBuffer), "SUB:%s", channelName);
  sendUdpCommand(messageBuffer);
}

void publishMessage(const char* channelName, const char* payload) {
  snprintf(messageBuffer, sizeof(messageBuffer), "PUB:%s:%s", channelName, payload);
  sendUdpCommand(messageBuffer);
}

void handleIncomingUdpPackets() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    Serial.print("[UDP] Received unicast packet, Size: ");
    Serial.print(packetSize);
    Serial.print(", From: ");
    Serial.print(udp.remoteIP());
    Serial.print(":");
    Serial.println(udp.remotePort());

    int len = (packetSize < 254) ? packetSize : 254;
    udp.read(packetBuffer, len);
    packetBuffer[len] = 0;
    
    Serial.print("[UDP] Contents: ");
    Serial.println(packetBuffer);
    // TODO: Add logic here to process the received packetBuffer
    // e.g., if it's a command for the Arduino on "arduino_commands" channel.
  }
}

void periodicallyPublishData() {
  if (millis() - lastPublishTime > PUBLISH_INTERVAL_MS) {
    publishMessage("sensor_data", "Calvin Says Hi"); // Example payload
    lastPublishTime = millis();
  }
}

// --- Utility ---
void printWiFiStatus(wl_status_t status) { // Kept for connectToWiFi if needed, but not directly used in refactor
  // ... (implementation from previous version) ...
}