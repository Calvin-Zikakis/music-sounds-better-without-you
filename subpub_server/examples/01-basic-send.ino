// Zerver UDP PubSub Client Example (Sustainable Arpeggiator)
#include <WiFi.h>
#include <WiFiUdp.h>

// --- Configuration ---
const char* WIFI_SSID = "Idea Fab Labs";
const char* WIFI_PASSWORD = "vortexrings";
const char* MULTICAST_ADDR = "239.0.0.100";
const int MULTICAST_PORT = 50100;
const int LOCAL_PORT = 7877;
const unsigned long PUBLISH_INTERVAL = 500; // ms

// --- State ---
WiFiUDP udp;
IPAddress serverIP;
int serverPort = 0;
unsigned long last_publish = 0;
int notes[] = {60, 64, 67, 72}; // C-major arpeggio
int note_index = 0;

// --- Forward Declarations ---
void connectWiFi();
bool discoverServer();
void publishArpeggioNote();

void setup() {
  Serial.begin(115200);
  Serial.println("Client starting...");
  connectWiFi();
  discoverServer();
}

void loop() {
  if (serverPort == 0) { // If server not found, retry discovery
    if (!discoverServer()) {
      delay(5000);
      return;
    }
  }

  if (millis() - last_publish >= PUBLISH_INTERVAL) {
    publishArpeggioNote();
    last_publish = millis();
  }
  delay(1);
}

void publishArpeggioNote() {
  // Prepare the message payload
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"note\":%d, \"vel\":100}", notes[note_index]);
  // Send the message to the server
  char message[128];
  snprintf(message, sizeof(message), "PUB:controller/generic:%s", payload);
  udp.beginPacket(serverIP, serverPort);
  udp.print(message);
  udp.endPacket();

  Serial.printf("Sent: %s\n", message);
  note_index = (note_index + 1) % 4;
}

void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
}

bool discoverServer() {
  Serial.print("Discovering server...");
  IPAddress multicastIP;
  multicastIP.fromString(MULTICAST_ADDR);
  udp.beginMulticast(multicastIP, MULTICAST_PORT);
  udp.beginPacket(multicastIP, MULTICAST_PORT);
  udp.print("DISCOVER_SUBPUB_SERVER");
  udp.endPacket();

  unsigned long timeout = millis() + 3000;
  while (millis() < timeout) {
    if (udp.parsePacket()) {
      char buffer[64];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      buffer[len] = 0;
      const char* prefix = "SUBPUB_SERVER_AT: ";
      if (strncmp(buffer, prefix, strlen(prefix)) == 0) {
        char* serverInfo = buffer + strlen(prefix);
        char* ipStr = strtok(serverInfo, ":");
        char* portStr = strtok(NULL, ":");
        if (ipStr && portStr && serverIP.fromString(ipStr)) {
          serverPort = atoi(portStr);
          udp.stop();
          udp.begin(LOCAL_PORT);
          Serial.printf("\nServer found: %s:%d\n", serverIP.toString().c_str(), serverPort);
          return true;
        }
      }
    }
    delay(10);
  }

  udp.stop();
  udp.begin(LOCAL_PORT);
  Serial.println("\nDiscovery failed.");
  return false;
}
