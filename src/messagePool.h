#ifndef MESSAGEPOOL_H
#define MESSAGEPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "whad.pb.h"
#include "messageQueue.h"

#define MESSAGE_POOL_MESSAGE_COUNT 8
#define MESSAGE_POOL_QUEUE_NODE_COUNT 24
#define MESSAGE_POOL_WRAPPER_COUNT 8
#define MESSAGE_POOL_PACKET_BUFFER_COUNT 4
#define MESSAGE_POOL_PACKET_PAYLOAD_COUNT 4
#define MESSAGE_POOL_COMMAND_RESPONSE_RESERVE 4
#define MESSAGE_POOL_RADIO_MAX_PACKET_SIZE 257
#define MESSAGE_POOL_PACKET_SLOT_SIZE 288
#define MESSAGE_POOL_WRAPPER_SLOT_SIZE 512

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MessagePoolStatus {
	MESSAGE_POOL_OK = 0,
	MESSAGE_POOL_EXHAUSTED,
	MESSAGE_POOL_OVERSIZE,
	MESSAGE_POOL_NOT_OWNED,
	MESSAGE_POOL_DOUBLE_FREE,
	MESSAGE_POOL_STALE_HANDLE
} MessagePoolStatus;

typedef enum MessagePoolKind {
	MESSAGE_POOL_KIND_NONE = 0,
	MESSAGE_POOL_KIND_MESSAGE,
	MESSAGE_POOL_KIND_QUEUE_NODE,
	MESSAGE_POOL_KIND_WRAPPER,
	MESSAGE_POOL_KIND_PACKET_BUFFER,
	MESSAGE_POOL_KIND_PACKET_PAYLOAD
} MessagePoolKind;

typedef enum MessagePoolTrafficClass {
	MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE = 0,
	MESSAGE_POOL_TRAFFIC_STREAM_EVENT
} MessagePoolTrafficClass;

typedef struct MessagePoolHandle {
	uint16_t index;
	uint16_t generation;
	uint8_t kind;
} MessagePoolHandle;

typedef struct MessagePoolConfig {
	size_t message_size;
	size_t queue_node_size;
	size_t wrapper_slot_size;
	size_t packet_slot_size;
	uint16_t message_count;
	uint16_t queue_node_count;
	uint16_t wrapper_count;
	uint16_t packet_buffer_count;
	uint16_t packet_payload_count;
	uint16_t command_response_reserve;
} MessagePoolConfig;

void messagePoolReset(void);
const MessagePoolConfig *messagePoolGetConfig(void);

Message *messagePoolAllocateMessage(MessagePoolHandle *handle);
MessagePoolStatus messagePoolReleaseMessage(Message *message);
MessagePoolHandle messagePoolHandleForMessage(Message *message);
MessagePoolStatus messagePoolReleaseMessageHandle(MessagePoolHandle handle);

MessageQueueElement *messagePoolAllocateQueueNode(MessagePoolTrafficClass trafficClass, MessagePoolHandle *handle);
MessagePoolStatus messagePoolReleaseQueueNode(MessageQueueElement *node);

void *messagePoolAllocateWrapper(size_t size);
MessagePoolStatus messagePoolReleaseWrapper(void *wrapper);

uint8_t *messagePoolAllocatePacketBuffer(size_t size);
MessagePoolStatus messagePoolReleasePacketBuffer(uint8_t *buffer);
uint8_t *messagePoolAllocatePacketPayload(size_t size);
MessagePoolStatus messagePoolReleasePacketPayload(uint8_t *payload);
bool messagePoolPacketSizeFits(size_t size);

uint32_t messagePoolOverflowCount(void);
void messagePoolRecordOverflow(void);

#ifdef __cplusplus
}
#endif

#endif
