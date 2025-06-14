/*
  PubSubClient.cpp - A simple client for MQTT.
  Nick O'Leary
  http://knolleary.net
  FINAL ARCHITECTURE v2: Corrected implementations and initialization.
*/

#include "PubSubClient.h"
#include "Arduino.h"

PubSubClient::PubSubClient()
{
    this->_state = MQTT_DISCONNECTED;
    this->_client = NULL;
    this->stream = NULL;
    this->buffer = NULL;

    this->bufferSize = 0;
    this->incomingQueue = xQueueCreate(10, sizeof(MqttIncomingMessage *));

    // >> ИСПРАВЛЕНИЕ: Прямое присваивание вместо вызова сеттеров в конструкторе
    this->keepAlive = MQTT_KEEPALIVE;
    this->socketTimeout = MQTT_SOCKET_TIMEOUT;

    setBufferSize(MQTT_MAX_PACKET_SIZE);
}

PubSubClient::~PubSubClient()
{
    if (this->buffer)

    {
        free(this->buffer);
    }
    if (this->incomingQueue)

    {
        vQueueDelete(this->incomingQueue);
    }
}

QueueHandle_t PubSubClient::getIncomingQueue() const
{
    return this->incomingQueue;
}

/**
 * Главный цикл обработки. Теперь он сначала пытается отправить сообщение из очереди,
 * а затем обрабатывает входящие данные.
 */
boolean PubSubClient::loop()
{
    if (connected())
    {
        // Отправить одно сообщение из очереди, если оно есть
        sendFromQueue();

        // Проверка keep-alive
        unsigned long t = millis();
        if ((t - lastInActivity > this->keepAlive * 1000UL) || (t - lastOutActivity > this->keepAlive * 1000UL))
        {
            if (pingOutstanding)
            {
                this->_state = MQTT_CONNECTION_TIMEOUT;
                _client->stop();
                return false;
            }
            else
            {
                this->buffer[0] = MQTTPINGREQ;
                this->buffer[1] = 0;
                if (this->_client->write(this->buffer, 2))
                {
                    lastOutActivity = t;
                    lastInActivity = t;
                    pingOutstanding = true;
                }
            }
        }

        // Обработка входящих данных
        if (_client->available())
        {
            uint8_t llen;
            uint16_t len = readPacket(&llen);

            if (len > 0)
            {
                lastInActivity = millis();
                uint8_t type = this->buffer[0] & 0xF0;
                if (type == MQTTPUBLISH)
                {
                    if (this->incomingQueue != NULL)
                    {
                        uint16_t topicLen = (this->buffer[llen + 1] << 8) + this->buffer[llen + 2];
                        char *topic = (char *)this->buffer + llen + 3;
                        uint8_t *payload;
                        uint16_t payloadLen;

                        if ((this->buffer[0] & 0x06) == MQTTQOS1)
                        {
                            uint16_t msgId = (this->buffer[llen + 3 + topicLen] << 8) + this->buffer[llen + 3 + topicLen + 1];
                            payload = this->buffer + llen + 3 + topicLen + 2;
                            payloadLen = len - llen - 3 - topicLen - 2;

                            this->buffer[0] = MQTTPUBACK;
                            this->buffer[1] = 2;
                            this->buffer[2] = (msgId >> 8);
                            this->buffer[3] = (msgId & 0xFF);
                            _client->write(this->buffer, 4);
                            lastOutActivity = millis();
                        }
                        else
                        {
                            payload = this->buffer + llen + 3 + topicLen;
                            payloadLen = len - llen - 3 - topicLen;
                        }
                        // 1. Создаем объект в КУЧЕ и получаем на него указатель
                        auto *msg = new MqttIncomingMessage();

                        // 2. Заполняем его данными, как и раньше
                        if (topicLen < sizeof(msg->topic))
                        {
                            strncpy(msg->topic, topic, topicLen);
                            msg->topic[topicLen] = '\0';
                        }
                        msg->payload.assign(payload, payload + payloadLen);

                        // 3. Отправляем в очередь сам УКАЗАТЕЛЬ
                        xQueueSend(this->incomingQueue, &msg, (TickType_t)0);
                    }
                }

                else if (type == MQTTPINGRESP)
                {
                    pingOutstanding = false;
                }
            }
        }
        return true;
    }
    return false;
}

/**
 * Новая приватная функция для отправки одного сообщения из очереди.
 * Возвращает true, если сообщение было отправлено.
 */
boolean PubSubClient::sendFromQueue()
{
    if (outgoingQueue.empty())
    {
        return false; // Очередь пуста, нечего отправлять
    }

    // Получаем сообщение из начала очереди, но пока не удаляем
    auto &msg = outgoingQueue.front();

    // Формируем и отправляем пакет в зависимости от типа сообщения
    bool result = false;
    switch (msg->type)
    {
    case MqttOutgoingPacketType::PUBLISH:
    {
        uint16_t topicLength = msg->topic.length();
        uint16_t payloadLength = msg->payload.size();
        if (MQTT_MAX_HEADER_SIZE + 2 + topicLength + payloadLength > this->bufferSize)
        {
            result = true;
            break;
        }
        uint16_t length = writeString(msg->topic.c_str(), this->buffer, 0);
        memcpy(this->buffer + length, msg->payload.data(), payloadLength);
        length += payloadLength;

        uint8_t header = MQTTPUBLISH;
        if (msg->retained)
        {
            header |= 1;
        }
        result = write(header, this->buffer, length);
        break;
    }
    case MqttOutgoingPacketType::SUBSCRIBE:
    {
        uint16_t length = 0;

        nextMsgId++;
        if (nextMsgId == 0)
            nextMsgId = 1;
        this->buffer[length++] = (nextMsgId >> 8);
        this->buffer[length++] = (nextMsgId & 0xFF);
        length = writeString(msg->topic.c_str(), this->buffer, length);
        this->buffer[length++] = msg->qos;
        result = write(MQTTSUBSCRIBE | MQTTQOS1, this->buffer, length);
        break;
    }
    case MqttOutgoingPacketType::UNSUBSCRIBE:
    {
        uint16_t length = 0;

        nextMsgId++;
        if (nextMsgId == 0)
            nextMsgId = 1;
        this->buffer[length++] = (nextMsgId >> 8);
        this->buffer[length++] = (nextMsgId & 0xFF);
        length = writeString(msg->topic.c_str(), this->buffer, length);
        result = write(MQTTUNSUBSCRIBE | MQTTQOS1, this->buffer, length);
        break;
    }
    }

    if (result)
    {
        outgoingQueue.pop();
    }
    return result;
}

boolean PubSubClient::publish(const char *topic, const uint8_t *payload, unsigned int plength, boolean retained)
{
    if (!connected() || !topic)

        return false;

    auto msg = std::make_unique<MqttOutgoingMessage>(topic, payload, plength, retained);
    outgoingQueue.push(std::move(msg));
    return true;
}

boolean PubSubClient::publish(const char *topic, const char *payload, boolean retained)
{
    return publish(topic, (const uint8_t *)payload, payload ? strlen(payload) : 0, retained);
}

boolean PubSubClient::subscribe(const char *topic, uint8_t qos)
{
    if (!connected() || !topic || qos > 2)

        return false;

    auto msg = std::make_unique<MqttOutgoingMessage>(topic, qos);
    outgoingQueue.push(std::move(msg));
    return true;
}

boolean PubSubClient::unsubscribe(const char *topic)
{
    if (!connected() || !topic)

        return false;

    auto msg = std::make_unique<MqttOutgoingMessage>(topic);
    outgoingQueue.push(std::move(msg));
    return true;
}

void PubSubClient::disconnect()
{

    this->buffer[0] = MQTTDISCONNECT;
    this->buffer[1] = 0;
    _client->write(this->buffer, 2);
    _state = MQTT_DISCONNECTED;
    _client->flush();
    _client->stop();
    lastInActivity = lastOutActivity = millis();
}

boolean PubSubClient::connect(const char *id, const char *user, const char *pass, const char *willTopic, uint8_t willQos, boolean willRetain, const char *willMessage, boolean cleanSession)

{
    if (connected())
        return true;
    int result = 0;
    if (_client->connected())

    {
        result = 1;
    }
    else

    {
        if (domain != NULL)
        {
            result = _client->connect(this->domain, this->port);
        }
        else
        {
            result = _client->connect(this->ip, this->port);
        }
    }
    if (result == 1)

    {
        nextMsgId = 1;
        uint16_t payloadLength = 0;
        if (id)
            payloadLength += 2 + strlen(id);
        if (willTopic)
        {
            payloadLength += 2 + strlen(willTopic);
            if (willMessage)
                payloadLength += 2 + strlen(willMessage);
        }
        if (user)
        {
            payloadLength += 2 + strlen(user);
            if (pass)
                payloadLength += 2 + strlen(pass);
        }
        uint16_t remainingLength = 10 + payloadLength;
        if (MQTT_MAX_HEADER_SIZE + remainingLength > this->bufferSize)
        {
            _client->stop();
            return false;
        }
        uint16_t pos = 0;
        this->buffer[pos++] = MQTTCONNECT;
        pos += buildHeader(0, this->buffer + pos, remainingLength);
        pos = writeString("MQTT", this->buffer, pos);
        this->buffer[pos++] = MQTT_VERSION;
        uint8_t v = 0;
        if (willTopic)
        {
            v = 0x04 | (willQos << 3) | (willRetain << 5);
        }
        if (cleanSession)
        {
            v = v | 0x02;
        }
        if (user)
        {
            v = v | 0x80;
            if (pass)
            {
                v = v | 0x40;
            }
        }
        this->buffer[pos++] = v;
        this->buffer[pos++] = ((this->keepAlive) >> 8);
        this->buffer[pos++] = ((this->keepAlive) & 0xFF);
        if (id)
            pos = writeString(id, this->buffer, pos);
        if (willTopic)
        {
            pos = writeString(willTopic, this->buffer, pos);
            if (willMessage)
                pos = writeString(willMessage, this->buffer, pos);
        }
        if (user)
        {
            pos = writeString(user, this->buffer, pos);
            if (pass)
                pos = writeString(pass, this->buffer, pos);
        }
        if (!_client->write(this->buffer, pos))
        {
            _client->stop();
            return false;
        }
        lastInActivity = lastOutActivity = millis();
        while (!_client->available())
        {
            unsigned long t = millis();
            if (t - lastInActivity >= ((unsigned long)this->socketTimeout * 1000UL))
            {
                _state = MQTT_CONNECTION_TIMEOUT;
                _client->stop();
                return false;
            }
        }
        uint8_t llen;
        uint32_t lenRead = readPacket(&llen);
        if (lenRead == 4 && buffer[0] == MQTTCONNACK)
        {
            if (buffer[3] == 0)
            {
                lastInActivity = millis();
                pingOutstanding = false;
                _state = MQTT_CONNECTED;
                return true;
            }
            else
            {
                _state = buffer[3];
            }
        }
        _client->stop();
    }
    else
    {
        _state = MQTT_CONNECT_FAILED;
    }
    return false;
}
uint32_t PubSubClient::readPacket(uint8_t *lengthLength)

{
    uint16_t len = 0;
    if (!readByte(this->buffer, &len))
        return 0;
    bool isPublish = (this->buffer[0] & 0xF0) == MQTTPUBLISH;
    uint32_t multiplier = 1;
    uint32_t length = 0;
    uint8_t digit = 0;
    uint16_t skip = 0;
    uint32_t start = 0;
    do
    {
        if (len == 5)
        {
            _client->stop();
            return 0;
        }
        if (!readByte(&digit))
            return 0;
        this->buffer[len++] = digit;
        length += (digit & 127) * multiplier;
        multiplier *= 128;
    } while ((digit & 128) != 0);
    *lengthLength = len - 1;
    if (isPublish)
    {
        if (!readByte(this->buffer, &len))
            return 0;
        if (!readByte(this->buffer, &len))
            return 0;
        skip = (this->buffer[*lengthLength + 1] << 8) + this->buffer[*lengthLength + 2];
        start = 2;
        if ((this->buffer[0] & 0x06) == MQTTQOS1)
        {
            skip += 2;
        }
    }
    for (uint32_t i = start; i < length; i++)
    {
        if (!readByte(&digit))
            return 0;
        if (this->stream && isPublish && len - (*lengthLength + 2) > skip)
        {
            this->stream->write(digit);
        }
        if (len < this->bufferSize)
        {
            this->buffer[len++] = digit;
        }
    }
    return len;
}
boolean PubSubClient::readByte(uint8_t *result)

{
    uint32_t previousMillis = millis();
    while (!_client->available())
    {
        yield();
        uint32_t currentMillis = millis();
        if (currentMillis - previousMillis >= ((int32_t)this->socketTimeout * 1000))
            return false;
    }
    *result = _client->read();
    return true;
}

// >> ИСПРАВЛЕНИЕ: Реализация недостающей функции
boolean PubSubClient::readByte(uint8_t *result, uint16_t *index)
{
    uint16_t current_index = *index;
    if (readByte(&result[current_index]))
    {
        *index = current_index + 1;
        return true;
    }
    return false;
}

boolean PubSubClient::write(uint8_t header, uint8_t *buf, uint16_t length)
{
    uint8_t lenBuf[4];
    uint8_t llen = 0;
    uint8_t digit;

    uint16_t len = length;
    do
    {
        digit = len & 127; // digit = len %128
        len >>= 7;         // len = len / 128
        if (len > 0)
        {
            digit |= 0x80;
        }
        lenBuf[llen++] = digit;

    } while (len > 0);
    uint8_t fixedHeader[MQTT_MAX_HEADER_SIZE];
    fixedHeader[0] = header;
    memcpy(fixedHeader + 1, lenBuf, llen);
    uint16_t writeSize = 1 + llen;
    if (_client->write(fixedHeader, writeSize) != writeSize)
        return false;
    if (length > 0)
    {
        if (_client->write(buf, length) != length)
            return false;
    }
    lastOutActivity = millis();
    return true;
}
size_t PubSubClient::buildHeader(uint8_t header, uint8_t *buf, uint16_t length)

{
    uint8_t lenBuf[4];
    uint8_t llen = 0;
    uint8_t digit;
    uint16_t len = length;
    do

    {
        digit = len % 128;
        len /= 128;
        if (len > 0)
        {
            digit |= 0x80;
        }
        lenBuf[llen++] = digit;
    } while (len > 0);
    memcpy(buf, lenBuf, llen);
    return llen;
}

uint16_t PubSubClient::writeString(const char *string, uint8_t *buf, uint16_t pos)
{
    const char *idp = string;
    uint16_t i = 0;
    uint16_t start = pos;
    pos += 2;
    while (*idp)
    {
        buf[pos++] = *idp++;
        i++;
    }
    buf[start] = (i >> 8);
    buf[start + 1] = (i & 0xFF);
    return pos;
}

boolean PubSubClient::connected()
{

    if (_client == NULL)
        return false;
    if (!_client->connected())
    {
        if (this->_state == MQTT_CONNECTED)

        {
            this->_state = MQTT_CONNECTION_LOST;
            _client->flush();
            _client->stop();
        }
        return false;
    }
    return this->_state == MQTT_CONNECTED;
}
int PubSubClient::state() { return this->_state; }

PubSubClient &PubSubClient::setServer(IPAddress ip, uint16_t port)
{
    this->ip = ip;
    this->port = port;
    this->domain = NULL;
    return *this;
}

PubSubClient &PubSubClient::setServer(const char *domain, uint16_t port)
{
    this->domain = domain;
    this->port = port;
    return *this;
}

PubSubClient &PubSubClient::setClient(Client &client)
{
    this->_client = &client;
    return *this;
}

PubSubClient &PubSubClient::setStream(Stream &stream)
{
    this->stream = &stream;
    return *this;
}
PubSubClient &PubSubClient::setKeepAlive(uint16_t keepAlive)
{
    this->keepAlive = keepAlive;
    return *this;
}
PubSubClient &PubSubClient::setSocketTimeout(uint16_t timeout)
{
    this->socketTimeout = timeout;
    return *this;
}

boolean PubSubClient::setBufferSize(uint16_t size)
{
    if (size == 0)

        return false;
    if (this->buffer)

    {
        free(this->buffer);
    }
    this->buffer = (uint8_t *)malloc(size);
    this->bufferSize = size;
    return (this->buffer != NULL);
}

// >> ИСПРАВЛЕНИЕ: Реализация виртуальных методов write, чтобы класс не был абстрактным
size_t PubSubClient::write(uint8_t data)

{
    // Эта функция используется для потоковой публикации.
    // В нашей асинхронной модели она пока не поддерживается в полной мере.
    // Возвращаем 1 для совместимости с интерфейсом Print.
    return 1;
}
size_t PubSubClient::write(const uint8_t *buffer, size_t size)
{
    // Аналогично, пока не поддерживается.
    return size;
}