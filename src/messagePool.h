#ifndef MESSAGEPOOL_H
#define MESSAGEPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "whad.pb.h"
#include "messageQueue.h"

/* Forward-declare board_Message so BoardMessageSlot is defined without
 * pulling board.pb.h into every TU that includes this header. */
typedef struct _board_Message board_Message;

/* Typed slot: binary-compatible with Message's board arm.
 * Layout mirrors Message: which_msg at 0, board arm at 8.
 * Padding forces 8-byte alignment to match Message's union offset
 * (driven by ble_Message's uint64_t in the full Message union).
 * sizeof(BoardMessageSlot) (~944) vs sizeof(Message) (7008) — saves ~6 KB/slot.
 * Only safe for TX via whad_board_pack (writes only the board arm).
 * MUST NOT be used for RX decode (domain unknown on wire). */
typedef struct {
	pb_size_t which_msg;
	uint8_t _pad[6];
	board_Message board;
} BoardMessageSlot;

#define MESSAGE_POOL_FULL_UNION_COUNT 2
#define MESSAGE_POOL_BOARD_SLOT_COUNT 6
#define MESSAGE_POOL_QUEUE_NODE_COUNT 24
#define MESSAGE_POOL_PACKET_BUFFER_COUNT 4
#define MESSAGE_POOL_PACKET_PAYLOAD_COUNT 4
#define MESSAGE_POOL_COMMAND_RESPONSE_RESERVE 4
#define MESSAGE_POOL_RADIO_MAX_PACKET_SIZE 257
#define MESSAGE_POOL_PACKET_SLOT_SIZE 288

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
	MESSAGE_POOL_KIND_BOARD_SLOT,
	MESSAGE_POOL_KIND_QUEUE_NODE,
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
/* Returned pointer MUST only be used with the matching domain's pack
 * functions — the slot is sized for that domain only (e.g. board arm
 * only). Never pass to RX decode or a different domain's pack function. */
Message *messagePoolAllocateForDomain(uint32_t domain);
MessagePoolStatus messagePoolReleaseMessage(Message *message);
MessagePoolHandle messagePoolHandleForMessage(Message *message);
MessagePoolStatus messagePoolReleaseMessageHandle(MessagePoolHandle handle);

MessageQueueElement *messagePoolAllocateQueueNode(MessagePoolTrafficClass trafficClass, MessagePoolHandle *handle);
MessagePoolStatus messagePoolReleaseQueueNode(MessageQueueElement *node);

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
