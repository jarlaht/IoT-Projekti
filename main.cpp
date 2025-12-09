#include "ESP8266Interface.h"
#include "mbed.h"
#include "ntp-client/NTPClient.h"
#include "parson.h"
#include <MQTTClientMbedOs.h>

#define BUFFER_LEN 256 // Payload length
#define ntpAddress "time.mikes.fi" // The VTT Mikes in Helsinki
#define ntpPort 123

// SPI for temperature sensor
SPI spi(D11, D12, D13); // mosi, miso, sclk
DigitalOut tc1(D6);

int rawLow = 0;
int rawHigh = 0;
int temperature = 0;
int temperatureDigits = 0;
int circuitTemperature = 0.0;
int openCircuit = 0;
int shortGND = 0;
int shortVCC = 0;

short tresholdMin = INT16_MAX;
short tresholdMax = INT16_MAX;
short criticalTresholdMin = INT16_MAX;
short criticalTresholdMax = INT16_MAX;

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
  while (true) {
    client->yield(100);
  }
}

int getTemperature(int bits) {
  // Negative temperature
  if ((bits >> 15) % 2) {
    // TODO
    return -1;
  }
  // Positive temperature
  else {
    return (bits >> 4);
  }
}

int getTemperatureDigits(int bits) {
    // Negative temperature
    if ((bits >> 15) % 2) {
      return 0;
    }
    // Positive temperature
    else {
      return ((bits >> 2) % 4) * 25;
    }
}

float getCircuitTemperature(int bits) {
    // TODO
    return -1;
}

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
  time_t timestamp = ntp.get_timestamp();

  printf("\ntimestamp %u\n", timestamp);
  set_time(timestamp);
  // TODO: check that timestamp is ok (> 0), refetch if necessary

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

  client.subscribe(MBED_CONF_APP_MQTT_UPDATE_TOPIC, MQTT::QOS0, messageArrived);
  tresholdUpdate.start(callback(tresholdUpdateArrived, &client));

  while (true) {
    tc1.write(0);
    ThisThread::sleep_for(1ms); // > 100ns for MAX31855

    // Send dummy to read
    rawHigh = spi.write(0x0000);
    rawLow = spi.write(0x0000);

    ThisThread::sleep_for(1ms);

    // Deselect
    tc1.write(1);

    tempFault = rawHigh % 2;
    openCircuit = rawLow % 2;
    shortGND = (rawLow >> 1) % 2;
    shortVCC = (rawLow >> 2) % 2;

    temperature = getTemperature(rawHigh);
    temperatureDigits = getTemperatureDigits(rawHigh);
    
    sprintf(buffer,
            "{\"time\": %d, \"temperature\": %d.%02d, \"tresholdMin\": %d, "
            "\"tresholdMax\": %d, \"criticalTresholdMin\": %d, "
            "\"criticalTresholdMax\": %d, \"tempFault\": %d}",
            time(NULL), temperature, temperatureDigits, tresholdMin,
            tresholdMax, criticalTresholdMin, criticalTresholdMax, tempFault);
    
    msg.payloadlen = strlen(buffer);
    printf("payload len %d", strlen(buffer));
    client.publish(MBED_CONF_APP_MQTT_TOPIC, msg);

    // client.yield(100);
    ThisThread::sleep_for(5s);
  }
  // client.yield(100);
  printf("Disconnect client\n");
  client.disconnect();
}
