#include "ESP8266Interface.h"
#include "mbed.h"
#include "ntp-client/NTPClient.h"
#include "parson.h"
#include <MQTTClientMbedOs.h>

#define BUFFER_LEN 256             // Payload length
#define ntpAddress "time.mikes.fi" // The VTT Mikes in Helsinki
#define ntpPort 123

// SPI for temperature sensor
SPI spi(D11, D12, D13); // mosi, miso, sclk
DigitalOut tc1(D6);

uint32_t rawLow = 0;
uint32_t rawHigh = 0;
uint32_t raw = 0;
int16_t temperature = 0;
int16_t temperatureDecimals = 0;
int16_t circuitTemperature = 0;
bool openCircuit = 0;
bool shortGND = 0;
bool shortVCC = 0;

int16_t tresholdMin = INT16_MAX;
int16_t tresholdMax = INT16_MAX;
int16_t criticalTresholdMin = INT16_MAX;
int16_t criticalTresholdMax = INT16_MAX;

bool tempFault = 0;

Thread tresholdUpdate;

void messageArrived(MQTT::MessageData &md) {
  MQTT::Message &msg = md.message;

  printf("\nMessage arrived:\n");
  printf("Topic: %.*s\n", md.topicName.lenstring.len,
         md.topicName.lenstring.data);

  printf("Payload: %.*s\n", msg.payloadlen, (char *)msg.payload);

  char buffer[256];
  int len = msg.payloadlen;
  if (len > 511)
    len = 511;
  memcpy(buffer, msg.payload, len);
  buffer[len] = '\0';

  // -------------------------------
  //  Decode JSON using Parson
  // -------------------------------
  JSON_Value *root = json_parse_string(buffer);
  if (root == NULL) {
    printf("JSON parse failed!\n");
    return;
  }

  JSON_Object *obj = json_value_get_object(root);

  tresholdMin = json_object_get_number(obj, "tresholdMin");
  tresholdMax = json_object_get_number(obj, "tresholdMax");
  criticalTresholdMin = json_object_get_number(obj, "criticalTresholdMin");
  criticalTresholdMax = json_object_get_number(obj, "criticalTresholdMax");
}

void tresholdUpdateArrived(MQTTClient *client) {
  client -> subscribe(MBED_CONF_APP_MQTT_UPDATE_TOPIC, MQTT::QOS0, messageArrived);
  while (true) {
    client->yield(100);
  }
}

int16_t getSignedTemperature(uint32_t raw) {
  int16_t tc14 = (raw >> 18) & 0x3FFF; // 0x3FFF = 0011 1111 1111 1111

  // sign extend 14-bit
  if (tc14 & 0x2000) // check sign bit
  {
    tc14 |= 0xC000; // set bits 15 and 14 for sign
  }
  return tc14;
}

int16_t getSigned14bit(int16_t raw) {
  int16_t tc14 = raw >> 2;

  // check sign bit
  if (tc14 & 0x2000) {
    // set bits 15 and 14 for sign
    tc14 |= 0xC000;
  }

  return tc14;
}

int16_t getTempInt(int16_t tc14) { return tc14 >> 2; }

int16_t getTempDec(int16_t tc14) { return (tc14 & 0x03) * 25; }
// int16_t getTemperatureInteger(int16_t tc14) { return tc14 >> 2; }

// int16_t getTemperatureDecimals(int16_t tc14) { return (tc14 & 0x03) * 25; }

float getCircuitTemperature(int bits) { return bits >> 8; }

int main() {
  // Temp sensor chip must be deselected
  tc1.write(1);

  // Setup the spi for 16bit data
  spi.format(16, 0);
  spi.frequency(1000000);
  ThisThread::sleep_for(100ms);

  // WIFI
  ESP8266Interface esp(MBED_CONF_APP_ESP_TX_PIN, MBED_CONF_APP_ESP_RX_PIN);

  // Store device IP
  SocketAddress deviceIP;
  // Store broker IP
  SocketAddress MQTTBroker;

  TCPSocket socket;
  MQTTClient client(&socket);

  printf("\nConnecting wifi..\n");

  int ret = esp.connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD,
                        NSAPI_SECURITY_WPA_WPA2);

  if (ret != 0) {
    printf("\nConnection error\n");
  } else {
    printf("\nConnection success\n");
  }

  esp.get_ip_address(&deviceIP);
  printf("IP via DHCP: %s\n", deviceIP.get_ip_address());

  // NTP client
  NTPClient ntp(&esp);
  ntp.set_server(ntpAddress, ntpPort);

  int timestamp = -1;

  while (timestamp < 0) {
    printf("Getting time from NTP server...\n");
    timestamp = ntp.get_timestamp();
  }

  printf("\ntimestamp %d\n", timestamp);
  set_time(timestamp);

  // MQTT
  // Use with IP
  // SocketAddress MQTTBroker(MBED_CONF_APP_MQTT_BROKER_IP,
  // MBED_CONF_APP_MQTT_BROKER_PORT);

  // Use with DNS
  esp.gethostbyname(MBED_CONF_APP_MQTT_BROKER_HOSTNAME, &MQTTBroker, NSAPI_IPv4,
                    "esp");
  MQTTBroker.set_port(MBED_CONF_APP_MQTT_BROKER_PORT);

  MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
  data.MQTTVersion = 3;
  char *id = MBED_CONF_APP_MQTT_ID;
  data.clientID.cstring = id;

  char buffer[BUFFER_LEN];

  MQTT::Message msg;
  msg.qos = MQTT::QOS0;
  msg.retained = false;
  msg.dup = false;
  msg.payload = (void *)buffer;
  msg.payloadlen = strlen(buffer);

  socket.open(&esp);
  socket.connect(MQTTBroker);
  client.connect(data);

  tresholdUpdate.start(callback(tresholdUpdateArrived, &client));

  while (true) {
    tc1.write(0);
    ThisThread::sleep_for(1ms); // > 100ns for MAX31855

    // Send dummy to read
    rawHigh = spi.write(0x0000);
    rawLow = spi.write(0x0000);

    raw = (rawHigh << 16) | rawLow;

    printf("rawHigh: %X\n", rawHigh);
    printf("rawLow: %X\n", rawLow);

    ThisThread::sleep_for(1ms);

    // Deselect
    tc1.write(1);

    tempFault = rawHigh % 2;
    openCircuit = rawLow % 2;
    shortGND = (rawLow >> 1) % 2;
    shortVCC = (rawLow >> 2) % 2;

    int16_t tc14 = getSigned14bit(rawHigh);
    temperature = getTempInt(tc14);
    temperatureDecimals = getTempDec(tc14);

    circuitTemperature = getCircuitTemperature(rawLow);

    printf("Circuit temp: %d\n", circuitTemperature);
    printf("openCircuit: %d\n", openCircuit);
    printf("short GND: %d\n", shortGND);
    printf("shortVCC = %d\n", shortVCC);

    sprintf(buffer,
            "{\"time\": %d, \"temperature\": %d.%d, \"tresholdMin\": %d, "
            "\"tresholdMax\": %d, \"criticalTresholdMin\": %d, "
            "\"criticalTresholdMax\": %d, \"tempFault\": %d}",
            time(NULL), temperature, temperatureDecimals, tresholdMin,
            tresholdMax, criticalTresholdMin, criticalTresholdMax, tempFault);

    msg.payloadlen = strlen(buffer);
    printf("Sending...\n");
    client.publish(MBED_CONF_APP_MQTT_TOPIC, msg);

    ThisThread::sleep_for(5s);
  }
  printf("Disconnect client\n");
  client.disconnect();
}
