/*
  PubSubClient.h - A simple client for MQTT.
  Nick O'Leary
  http://knolleary.net
  FINAL ARCHITECTURE v2: Corrected declarations.
*/

#ifndef PubSubClient_h
#define PubSubClient_h

#include <Arduino.h>
#include "IPAddress.h"
#include "Client.h"
#include "Stream.h"
#include <string>
#include <queue>
#include <memory>
#include <vector>

#define MQTT_VERSION_3_1 3
#define MQTT_VERSION_3_1_1 4

// MQTT_VERSION : Pick the version
// #define MQTT_VERSION MQTT_VERSION_3_1
#ifndef MQTT_VERSION
#define MQTT_VERSION MQTT_VERSION_3_1_1
#endif

// MQTT_MAX_PACKET_SIZE : Maximum packet size. Override with setBufferSize().
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 256
#endif

// MQTT_KEEPALIVE : keepAlive interval in Seconds. Override with setKeepAlive()
#ifndef MQTT_KEEPALIVE
#define MQTT_KEEPALIVE 15
#endif

// MQTT_SOCKET_TIMEOUT: socket timeout interval in Seconds. Override with setSocketTimeout()
#ifndef MQTT_SOCKET_TIMEOUT
#define MQTT_SOCKET_TIMEOUT 15
#endif

// MQTT_MAX_TRANSFER_SIZE : limit how much data is passed to the network client
//  in each write call. Needed for the Arduino Wifi Shield. Leave undefined to
//  pass the entire MQTT packet in each write call.
// #define MQTT_MAX_TRANSFER_SIZE 80

// Possible values for client.state()
#define MQTT_CONNECTION_TIMEOUT         -4
#define MQTT_CONNECTION_LOST            -3
#define MQTT_CONNECT_FAILED             -2
#define MQTT_DISCONNECTED               -1
#define MQTT_CONNECTED                  0
#define MQTT_CONNECT_BAD_PROTOCOL       1
#define MQTT_CONNECT_BAD_CLIENT_ID      2
#define MQTT_CONNECT_UNAVAILABLE        3
#define MQTT_CONNECT_BAD_CREDENTIALS    4
#define MQTT_CONNECT_UNAUTHORIZED       5

#define MQTTCONNECT     1 << 4      // Client request to connect to Server
#define MQTTCONNACK     2 << 4      // Connect Acknowledgment
#define MQTTPUBLISH     3 << 4      // Publish message
#define MQTTPUBACK      4 << 4      // Publish Acknowledgment
#define MQTTPUBREC      5 << 4      // Publish Received (assured delivery part 1)
#define MQTTPUBREL      6 << 4      // Publish Release (assured delivery part 2)
#define MQTTPUBCOMP     7 << 4      // Publish Complete (assured delivery part 3)
#define MQTTSUBSCRIBE   8 << 4      // Client Subscribe request
#define MQTTSUBACK      9 << 4      // Subscribe Acknowledgment
#define MQTTUNSUBSCRIBE 10 << 4     // Client Unsubscribe request
#define MQTTUNSUBACK    11 << 4     // Unsubscribe Acknowledgment
#define MQTTPINGREQ     12 << 4     // PING Request
#define MQTTPINGRESP    13 << 4     // PING Response
#define MQTTDISCONNECT  14 << 4     // Client is Disconnecting
#define MQTTReserved    15 << 4     // Reserved

#define MQTTQOS0 (0 << 1)
#define MQTTQOS1 (1 << 1)
#define MQTTQOS2 (2 << 1)

// Maximum size of fixed header and variable length size header
#define MQTT_MAX_HEADER_SIZE 5

#if defined(ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
#error "This version of the library requires an RTOS like FreeRTOS (ESP32)"
#endif

// Типы пакетов, которые можно поместить в очередь
enum class MqttOutgoingPacketType
{
    PUBLISH,
    SUBSCRIBE,
    UNSUBSCRIBE
};

// Структура для хранения данных исходящего сообщения
// Копирует все данные, чтобы избежать проблем с временем жизни указателей
struct MqttOutgoingMessage
{
    MqttOutgoingPacketType type;
    std::string topic;
    std::vector<uint8_t> payload;
    uint8_t qos;
    bool retained;

    // Конструктор для PUBLISH
    MqttOutgoingMessage(const char *t, const uint8_t *p, unsigned int p_len, bool r)
        : type(MqttOutgoingPacketType::PUBLISH), topic(t), payload(p, p + p_len), qos(0), retained(r) {}

    // Конструктор для SUBSCRIBE
    MqttOutgoingMessage(const char *t, uint8_t q)
        : type(MqttOutgoingPacketType::SUBSCRIBE), topic(t), qos(q), retained(false) {}

    // Конструктор для UNSUBSCRIBE
    MqttOutgoingMessage(const char *t)
        : type(MqttOutgoingPacketType::UNSUBSCRIBE), topic(t), qos(0), retained(false) {}
};
struct MqttIncomingMessage
{
    char topic[128];
    std::vector<uint8_t> payload;
    MqttIncomingMessage() { memset(topic, 0, sizeof(topic)); }
};

class PubSubClient : public Print
{
private:
    Client *_client;
    uint8_t *buffer;
    uint16_t bufferSize;
    uint16_t keepAlive;
    uint16_t socketTimeout;
    uint16_t nextMsgId;
    unsigned long lastOutActivity;
    unsigned long lastInActivity;
    bool pingOutstanding;

    std::queue<std::unique_ptr<MqttOutgoingMessage>> outgoingQueue;
    QueueHandle_t incomingQueue;

    boolean sendFromQueue();
    uint32_t readPacket(uint8_t *);
    boolean readByte(uint8_t *result);
    boolean readByte(uint8_t *result, uint16_t *index);
    boolean write(uint8_t header, uint8_t *buf, uint16_t length);
    uint16_t writeString(const char *string, uint8_t *buf, uint16_t pos);
    // Build up the header ready to send
    // Returns the size of the header
    // Note: the header is built at the end of the first MQTT_MAX_HEADER_SIZE bytes, so will start
    //       (MQTT_MAX_HEADER_SIZE - <returned size>) bytes into the buffer
    size_t buildHeader(uint8_t header, uint8_t *buf, uint16_t length);
    IPAddress ip;
    const char *domain;
    uint16_t port;
    Stream *stream;
    int _state;

public:
    PubSubClient();
    ~PubSubClient();

    QueueHandle_t getIncomingQueue() const;

    PubSubClient &setServer(IPAddress ip, uint16_t port);
    PubSubClient &setServer(const char *domain, uint16_t port);
    PubSubClient &setClient(Client &client);
    PubSubClient &setStream(Stream &stream);
    PubSubClient &setKeepAlive(uint16_t keepAlive);
    PubSubClient &setSocketTimeout(uint16_t timeout);

    boolean setBufferSize(uint16_t size);

    boolean connect(const char *id, const char *user = nullptr, const char *pass = nullptr, const char *willTopic = nullptr, uint8_t willQos = 0, boolean willRetain = false, const char *willMessage = nullptr, boolean cleanSession = true);
    void disconnect();
    boolean publish(const char *topic, const char *payload, boolean retained = false);
    boolean publish(const char *topic, const uint8_t *payload, unsigned int plength, boolean retained = false);
    // Write a single byte of payload (only to be used with beginPublish/endPublish)
    virtual size_t write(uint8_t);
    // Write size bytes from buffer into the payload (only to be used with beginPublish/endPublish)
    // Returns the number of bytes written
    virtual size_t write(const uint8_t *buffer, size_t size);
    boolean subscribe(const char *topic, uint8_t qos = 0);
    boolean unsubscribe(const char *topic);
    boolean loop();
    boolean connected();
    int state();
};

#endif
