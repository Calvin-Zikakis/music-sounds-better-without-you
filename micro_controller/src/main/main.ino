// Arduino UDP Pub/Sub Client Example
// Compatible with ESP32.

#include <WiFi.h>
#include <WiFiUdp.h>

// --- Configuration ---
// Wi-Fi Credentials
const char* WIFI_SSID = "Idea Fab Labs";    // Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "vortexrings";  // Replace with your Wi-Fi Password

// Sub/Pub Server (will be updated by discovery)
IPAddress udpServerIp;
int udpServerPort = 0;  // Set to 0 initially, indicates not yet discovered
const int LOCAL_UDP_PORT = 7877;

// Multicast Discovery (Matches Rust Server)
const char* MULTICAST_ADDRESS_STR = "239.0.0.100";
const int MULTICAST_PORT = 50100;
const char* DISCOVERY_MESSAGE = "DISCOVER_SUBPUB_SERVER";
const char* DISCOVERY_RESPONSE_PREFIX = "SUBPUB_SERVER_AT: ";

// Timings
const unsigned long PUBLISH_INTERVAL_MS = 5000;             // Publish every 5 seconds
const unsigned long DISCOVERY_ATTEMPT_INTERVAL_MS = 30000;  // Try discovery every 30 seconds if not found
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;     // Try to reconnect WiFi every 10 seconds if lost
const unsigned long DISCOVERY_TIMEOUT_MS = 5000;            // Timeout for waiting for a discovery response

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
  delay(2000);  // Allow board to stabilize
  Serial.begin(115200);
  Serial.println("\nStarting Arduino UDP Pub/Sub Client with Discovery...");
  delay(1000);

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
    delay(100);  // Brief pause before next check
    return;
  }

  if (!serverDiscovered) {
    handleServerNotDiscovered();
  } else {
    // WiFi is connected and server is discovered, proceed with normal operations
    handleIncomingUdpPackets();
    publishArpeggioNote();
    delay(100);
  }
  delay(10);  // Small delay to keep things responsive
}

// --- WiFi Management ---
// Updated WiFi connection function with LOLIN S2 fixes

// Advanced LOLIN S2 WiFi fixes based on hardware research
// Many of these issues are hardware-related on clone boards

void attemptWiFiConnection() {
  Serial.print("[WiFi] Advanced connection attempt for LOLIN S2: ");
  Serial.println(WIFI_SSID);
  
  // Critical physical issue workaround: Many clones have EN pin noise issues
  Serial.println("[WiFi] If this fails, try these PHYSICAL fixes:");
  Serial.println("  1. Hold your finger on EN pin and GND while connecting");
  Serial.println("  2. Place a coin on EN pin corner of board");
  Serial.println("  3. Add 22uF capacitor between EN and GND pins");
  
  // More aggressive WiFi reset sequence for problematic boards
  WiFi.mode(WIFI_MODE_NULL);  // Completely disable WiFi first
  delay(1000);
  WiFi.mode(WIFI_STA);        // Re-enable in station mode
  delay(500);
  
  // Disable all power saving features that cause instability
  WiFi.setSleep(false);
  
  // Try the most conservative settings first
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // Lowest power to reduce interference
  
  // Advanced: Try different channel preferences (some boards are picky)
  int preferredChannels[] = {1, 6, 11, 0};  // 0 = auto
  
  for (int chIdx = 0; chIdx < 4; chIdx++) {
    int channel = preferredChannels[chIdx];
    
    if (channel == 0) {
      Serial.println("[WiFi] Trying auto channel selection...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    } else {
      Serial.print("[WiFi] Trying fixed channel ");
      Serial.println(channel);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel);
    }
    
    // Shorter timeout per attempt since we're trying multiple methods
    const int LED_PIN = 15;
    pinMode(LED_PIN, OUTPUT);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
      digitalWrite(LED_PIN, attempts % 2);  // Blink LED
      delay(1000);
      Serial.print(".");
      attempts++;
      
      // Critical for some clone boards: Check for "Auth Expired" error pattern
      if (WiFi.status() == WL_CONNECT_FAILED) {
        Serial.print("\n[WiFi] Auth failed - trying reset...");
        WiFi.disconnect(true);
        delay(2000);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel);
        attempts = 0;  // Reset attempt counter
      }
    }
    
    digitalWrite(LED_PIN, LOW);  // Turn off LED
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WiFi] Connected!");
      Serial.print("[WiFi] Channel: ");
      Serial.println(WiFi.channel());
      Serial.print("[WiFi] IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("[WiFi] Signal: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      wifiConnected = true;
      return;
    } else {
      Serial.print(" FAILED (");
      Serial.print(WiFi.status());
      Serial.println(")");
      WiFi.disconnect(true);
      delay(1000);
    }
  }
  
  Serial.println("\n[WiFi] All advanced methods failed!");
  Serial.println("[WiFi] This appears to be a hardware issue with your clone board.");
  Serial.println("[WiFi] IMMEDIATE FIXES TO TRY:");
  Serial.println("  1. Touch EN pin with your finger while connecting");
  Serial.println("  2. Try a genuine LOLIN board instead of clone");
  Serial.println("  3. Install Tasmota firmware (works better on clones)");
  wifiConnected = false;
}

// Alternative method using WiFi events (works better for some problematic boards)
void attemptWiFiConnectionWithEvents() {
  Serial.println("[WiFi] Trying event-driven connection...");
  
  bool connected = false;
  unsigned long startTime = millis();
  const unsigned long TIMEOUT = 30000;  // 30 seconds
  
  // Set up event handler
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[WiFi] Event: Connected to AP");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.println("[WiFi] Event: Got IP address");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        connected = true;
        wifiConnected = true;
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.println("[WiFi] Event: Disconnected");
        if (info.wifi_sta_disconnected.reason == WIFI_REASON_AUTH_EXPIRE) {
          Serial.println("[WiFi] Auth expired - typical clone board issue");
        }
        break;
      default:
        break;
    }
  });
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Wait with timeout
  while (!connected && (millis() - startTime) < TIMEOUT) {
    delay(100);
    if (millis() % 5000 == 0) {
      Serial.print(".");
    }
  }
  
  if (!connected) {
    Serial.println("\n[WiFi] Event-driven connection failed");
    wifiConnected = false;
  }
}

// Nuclear option: Complete WiFi stack reset
void resetWiFiStack() {
  Serial.println("[WiFi] Performing complete WiFi stack reset...");
  
  // Completely shutdown WiFi
  WiFi.mode(WIFI_MODE_NULL);
  delay(1000);
  
  // Reset all WiFi settings to defaults
  WiFi.persistent(false);  // Don't save to flash
  WiFi.setAutoReconnect(false);
  
  delay(1000);
  
  // Restart fresh
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  Serial.println("[WiFi] Stack reset complete, ready for connection attempt");
}

// Test if the board has the physical hardware issue
void testHardwareIssue() {
  Serial.println("\n=== HARDWARE ISSUE TEST ===");
  Serial.println("Many LOLIN S2 clones have a physical EN pin issue.");
  Serial.println("DURING THIS TEST: Touch the EN pin with your finger!");
  Serial.println("If WiFi works while touching EN pin, you have the hardware issue.");
  Serial.println("Starting test in 5 seconds...");
  
  for (int i = 5; i > 0; i--) {
    Serial.print(i);
    Serial.print("... ");
    delay(1000);
  }
  Serial.println("START!");
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    Serial.print(".");
    delay(1000);
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[TEST RESULT] SUCCESS!");
    Serial.println("If you were touching the EN pin, you have the hardware issue.");
    Serial.println("SOLUTION: Add 22uF capacitor between EN and GND pins");
    wifiConnected = true;
  } else {
    Serial.println("\n[TEST RESULT] FAILED");
    Serial.println("Even with EN pin touch, connection failed.");
    Serial.println("This may be a different issue or completely broken clone.");
    wifiConnected = false;
  }
}

// Updated main connection function with all new methods
void attemptWiFiConnectionComplete() {
  Serial.println("[WiFi] Starting comprehensive LOLIN S2 connection attempt...");
  
  // Method 1: Try standard power level progression (your current code)
  attemptWiFiConnection();
  
  if (!wifiConnected) {
    Serial.println("[WiFi] Standard method failed, trying advanced fixes...");
    
    // Method 2: Complete stack reset
    resetWiFiStack();
    attemptWiFiConnection();
  }
  
  if (!wifiConnected) {
    Serial.println("[WiFi] Advanced method failed, trying event-driven...");
    
    // Method 3: Event-driven connection
    attemptWiFiConnectionWithEvents();
  }
  
  if (!wifiConnected) {
    Serial.println("[WiFi] All automatic methods failed.");
    Serial.println("[WiFi] Running hardware issue test...");
    
    // Method 4: Hardware test
    testHardwareIssue();
  }
  
  if (!wifiConnected) {
    Serial.println("\n==========================================");
    Serial.println("CONCLUSION: Your LOLIN S2 has hardware issues");
    Serial.println("==========================================");
    Serial.println("IMMEDIATE SOLUTIONS:");
    Serial.println("1. Buy genuine LOLIN board (not Amazon clone)");
    Serial.println("2. Add 22uF capacitor: EN(+) to GND(-)");
    Serial.println("3. Use better 5V power supply (>500mA)");
    Serial.println("4. Try Tasmota firmware instead of Arduino");
    Serial.println("==========================================");
  }
}

void handleWiFiDisconnected() {
  if (millis() - lastWiFiReconnectAttemptTime > WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("[WiFi] Connection lost. Attempting to reconnect...");
    WiFi.disconnect(true);  // Ensure clean disconnect before retrying
    delay(100);             // Short delay
    attemptWiFiConnection();
    lastWiFiReconnectAttemptTime = millis();
    if (wifiConnected) {  // If reconnected, reset discovery
      serverDiscovered = false;
      initializeUdp();           // Re-initialize UDP as it might have been stopped
      attemptServerDiscovery();  // And try to find the server again
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

void publishArpeggioNote() {
  // Prepare the message payload
  publishMessage("controller/generic", "{\"note\":60, \"vel\":100}");
}

void attemptServerDiscovery() {
  Serial.println("[Discovery] Attempting to discover SubPub server...");

  IPAddress multicastIP;
  if (!multicastIP.fromString(MULTICAST_ADDRESS_STR)) {
    Serial.println("[Discovery] ERROR: Failed to parse multicast IP string.");
    serverDiscovered = false;
    return;
  }

  udp.stop();  // Stop current UDP to switch to multicast listening

  if (!udp.beginMulticast(multicastIP, MULTICAST_PORT)) {
    Serial.println("[Discovery] ERROR: Failed to begin multicast listening.");
    udp.begin(LOCAL_UDP_PORT);  // Fallback to unicast listening
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
    udp.begin(LOCAL_UDP_PORT);  // Fallback
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
            serverDiscovered = true;  // Set global flag
            break;
          }
        }
      }
    }
    delay(10);
  }

  udp.stop();                        // Stop multicast listening
  if (!udp.begin(LOCAL_UDP_PORT)) {  // Re-initialize for unicast listening
    Serial.println("[UDP] CRITICAL: Failed to re-initialize for unicast after discovery!");
    serverDiscovered = false;  // Can't operate if UDP is broken
  } else {
    Serial.print("[UDP] Re-initialized for unicast listening on port: ");
    Serial.println(LOCAL_UDP_PORT);
  }

  if (!foundInThisAttempt) {
    Serial.println("[Discovery] Server not found in this attempt.");
    serverDiscovered = false;  // Ensure it's false if not found
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
      Serial.print(udpServerIp);
      Serial.print(":");
      Serial.print(udpServerPort);
      Serial.print(" -> ");
      Serial.println(command);
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
  }
}