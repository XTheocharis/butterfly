#include "messagePool.h"

#include <string.h>
#include "whad/protocol/board/board.pb.h"
#include "discovery.h"

#if defined(NRF52840_XXAA)
extern "C" void app_util_critical_region_enter(uint8_t *p_nested);
extern "C" void app_util_critical_region_exit(uint8_t nested);
#endif

static_assert(sizeof(Message) == 7008, "Message size drift — check nanopb regeneration");
static_assert(offsetof(Message, msg) == 8, "Message union offset drift");
static_assert(offsetof(BoardMessageSlot, which_msg) == offsetof(Message, which_msg),
	"BoardMessageSlot header mismatch");
static_assert(offsetof(BoardMessageSlot, board) == offsetof(Message, msg.board),
	"BoardMessageSlot arm offset mismatch");

typedef union PacketSlot {
	void *ptr;
	long double align;
	uint8_t bytes[MESSAGE_POOL_PACKET_SLOT_SIZE];
} PacketSlot;

typedef struct PoolState {
	uint16_t freeStack[MESSAGE_POOL_QUEUE_NODE_COUNT];
	uint16_t generation[MESSAGE_POOL_QUEUE_NODE_COUNT];
	bool inUse[MESSAGE_POOL_QUEUE_NODE_COUNT];
	uint16_t freeTop;
	uint16_t count;
} PoolState;

static Message g_messagePool[MESSAGE_POOL_FULL_UNION_COUNT];
static BoardMessageSlot g_boardSlotPool[MESSAGE_POOL_BOARD_SLOT_COUNT];
static MessageQueueElement g_queueNodePool[MESSAGE_POOL_QUEUE_NODE_COUNT];
static PacketSlot g_packetBufferPool[MESSAGE_POOL_PACKET_BUFFER_COUNT];
static PacketSlot g_packetPayloadPool[MESSAGE_POOL_PACKET_PAYLOAD_COUNT];

static PoolState g_messageState;
static PoolState g_boardSlotState;
static PoolState g_queueNodeState;
static PoolState g_packetBufferState;
static PoolState g_packetPayloadState;

static bool g_initialized = false;
static uint32_t g_overflowCount = 0;

static const MessagePoolConfig g_config = {
	sizeof(Message),
	sizeof(MessageQueueElement),
	0,
	MESSAGE_POOL_PACKET_SLOT_SIZE,
	MESSAGE_POOL_FULL_UNION_COUNT,
	MESSAGE_POOL_QUEUE_NODE_COUNT,
	0,
	MESSAGE_POOL_PACKET_BUFFER_COUNT,
	MESSAGE_POOL_PACKET_PAYLOAD_COUNT,
	MESSAGE_POOL_COMMAND_RESPONSE_RESERVE
};

typedef char message_pool_packet_slot_fits_radio_max[(MESSAGE_POOL_PACKET_SLOT_SIZE >= MESSAGE_POOL_RADIO_MAX_PACKET_SIZE) ? 1 : -1];
typedef char message_pool_packet_slot_fits_packet_payload[(MESSAGE_POOL_PACKET_SLOT_SIZE >= (MESSAGE_POOL_RADIO_MAX_PACKET_SIZE + 24)) ? 1 : -1];

static void poolInit(PoolState *state, uint16_t count)
{
	state->count = count;
	state->freeTop = count;
	for (uint16_t i = 0; i < count; i++) {
		state->freeStack[i] = (uint16_t)(count - 1 - i);
		state->generation[i] = 1;
		state->inUse[i] = false;
	}
}

static void ensureInitialized(void)
{
	if (!g_initialized) {
		messagePoolReset();
	}
}

static uint8_t enterCritical(void)
{
#if defined(NRF52840_XXAA)
	uint8_t nested = 0;
	app_util_critical_region_enter(&nested);
	return nested;
#else
	return 0;
#endif
}

static void exitCritical(uint8_t nested)
{
#if defined(NRF52840_XXAA)
	app_util_critical_region_exit(nested);
#else
	(void)nested;
#endif
}

static bool allocateIndex(PoolState *state, MessagePoolTrafficClass trafficClass, uint16_t *index)
{
	if (state->freeTop == 0) {
		return false;
	}

	if (trafficClass == MESSAGE_POOL_TRAFFIC_STREAM_EVENT &&
		state == &g_queueNodeState &&
		state->freeTop <= MESSAGE_POOL_COMMAND_RESPONSE_RESERVE) {
		return false;
	}

	state->freeTop--;
	*index = state->freeStack[state->freeTop];
	state->inUse[*index] = true;
	return true;
}

static MessagePoolStatus releaseIndex(PoolState *state, MessagePoolHandle handle)
{
	if (handle.index >= state->count) {
		return MESSAGE_POOL_NOT_OWNED;
	}
	if (state->generation[handle.index] != handle.generation) {
		return MESSAGE_POOL_STALE_HANDLE;
	}
	if (!state->inUse[handle.index]) {
		return MESSAGE_POOL_DOUBLE_FREE;
	}

	state->inUse[handle.index] = false;
	state->generation[handle.index]++;
	if (state->generation[handle.index] == 0) {
		state->generation[handle.index] = 1;
	}
	state->freeStack[state->freeTop] = handle.index;
	state->freeTop++;
	return MESSAGE_POOL_OK;
}

static MessagePoolHandle makeHandle(MessagePoolKind kind, uint16_t index, uint16_t generation)
{
	MessagePoolHandle handle;
	handle.kind = (uint8_t)kind;
	handle.index = index;
	handle.generation = generation;
	return handle;
}

static MessagePoolHandle nullHandle(MessagePoolKind kind)
{
	return makeHandle(kind, UINT16_MAX, 0);
}

template <typename Slot>
static MessagePoolHandle handleForSlot(PoolState *state, Slot *pool, void *ptr, MessagePoolKind kind)
{
	uintptr_t start = (uintptr_t)&pool[0];
	uintptr_t end = (uintptr_t)&pool[state->count];
	uintptr_t current = (uintptr_t)ptr;
	if (current < start || current >= end) {
		return nullHandle(kind);
	}
	uintptr_t offset = current - start;
	if ((offset % sizeof(Slot)) != 0) {
		return nullHandle(kind);
	}
	uint16_t index = (uint16_t)(offset / sizeof(Slot));
	return makeHandle(kind, index, state->generation[index]);
}

void messagePoolReset(void)
{
	uint8_t nested = enterCritical();
	poolInit(&g_messageState, MESSAGE_POOL_FULL_UNION_COUNT);
	poolInit(&g_boardSlotState, MESSAGE_POOL_BOARD_SLOT_COUNT);
	poolInit(&g_queueNodeState, MESSAGE_POOL_QUEUE_NODE_COUNT);
	poolInit(&g_packetBufferState, MESSAGE_POOL_PACKET_BUFFER_COUNT);
	poolInit(&g_packetPayloadState, MESSAGE_POOL_PACKET_PAYLOAD_COUNT);
	memset(g_messagePool, 0, sizeof(g_messagePool));
	memset(g_boardSlotPool, 0, sizeof(g_boardSlotPool));
	memset(g_queueNodePool, 0, sizeof(g_queueNodePool));
	memset(g_packetBufferPool, 0, sizeof(g_packetBufferPool));
	memset(g_packetPayloadPool, 0, sizeof(g_packetPayloadPool));
	g_overflowCount = 0;
	g_initialized = true;
	exitCritical(nested);
}

const MessagePoolConfig *messagePoolGetConfig(void)
{
	return &g_config;
}

Message *messagePoolAllocateMessage(MessagePoolHandle *handle)
{
	ensureInitialized();
	uint16_t index = 0;
	uint8_t nested = enterCritical();
	bool ok = allocateIndex(&g_messageState, MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, &index);
	exitCritical(nested);
	if (!ok) {
		messagePoolRecordOverflow();
		if (handle != NULL) {
			*handle = nullHandle(MESSAGE_POOL_KIND_MESSAGE);
		}
		return NULL;
	}
	memset(&g_messagePool[index], 0, sizeof(Message));
	if (handle != NULL) {
		*handle = makeHandle(MESSAGE_POOL_KIND_MESSAGE, index, g_messageState.generation[index]);
	}
	return &g_messagePool[index];
}

Message *messagePoolAllocateForDomain(uint32_t domain)
{
	ensureInitialized();
	if (domain == DOMAIN_BOARD) {
		uint16_t index = 0;
		uint8_t nested = enterCritical();
		bool ok = allocateIndex(&g_boardSlotState, MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, &index);
		exitCritical(nested);
		if (!ok) {
			messagePoolRecordOverflow();
			return NULL;
		}
		memset(&g_boardSlotPool[index], 0, sizeof(BoardMessageSlot));
		return reinterpret_cast<Message *>(&g_boardSlotPool[index]);
	}
	return messagePoolAllocateMessage(NULL);
}

MessagePoolHandle messagePoolHandleForMessage(Message *message)
{
	ensureInitialized();
	if (message >= &g_messagePool[0] && message < &g_messagePool[MESSAGE_POOL_FULL_UNION_COUNT]) {
		uint16_t index = (uint16_t)(message - &g_messagePool[0]);
		return makeHandle(MESSAGE_POOL_KIND_MESSAGE, index, g_messageState.generation[index]);
	}
	Message *boardStart = reinterpret_cast<Message *>(&g_boardSlotPool[0]);
	Message *boardEnd = reinterpret_cast<Message *>(&g_boardSlotPool[MESSAGE_POOL_BOARD_SLOT_COUNT]);
	if (message >= boardStart && message < boardEnd) {
		uint16_t index = (uint16_t)(message - boardStart);
		return makeHandle(MESSAGE_POOL_KIND_BOARD_SLOT, index, g_boardSlotState.generation[index]);
	}
	return nullHandle(MESSAGE_POOL_KIND_MESSAGE);
}

MessagePoolStatus messagePoolReleaseMessageHandle(MessagePoolHandle handle)
{
	ensureInitialized();
	PoolState *state;
	if (handle.kind == MESSAGE_POOL_KIND_BOARD_SLOT) {
		state = &g_boardSlotState;
	}
	else if (handle.kind == MESSAGE_POOL_KIND_MESSAGE) {
		state = &g_messageState;
	}
	else {
		return MESSAGE_POOL_NOT_OWNED;
	}
	uint8_t nested = enterCritical();
	MessagePoolStatus status = releaseIndex(state, handle);
	exitCritical(nested);
	return status;
}

MessagePoolStatus messagePoolReleaseMessage(Message *message)
{
	MessagePoolHandle handle = messagePoolHandleForMessage(message);
	return messagePoolReleaseMessageHandle(handle);
}

MessageQueueElement *messagePoolAllocateQueueNode(MessagePoolTrafficClass trafficClass, MessagePoolHandle *handle)
{
	ensureInitialized();
	uint16_t index = 0;
	uint8_t nested = enterCritical();
	bool ok = allocateIndex(&g_queueNodeState, trafficClass, &index);
	exitCritical(nested);
	if (!ok) {
		messagePoolRecordOverflow();
		if (handle != NULL) {
			*handle = nullHandle(MESSAGE_POOL_KIND_QUEUE_NODE);
		}
		return NULL;
	}
	memset(&g_queueNodePool[index], 0, sizeof(MessageQueueElement));
	if (handle != NULL) {
		*handle = makeHandle(MESSAGE_POOL_KIND_QUEUE_NODE, index, g_queueNodeState.generation[index]);
	}
	return &g_queueNodePool[index];
}

MessagePoolStatus messagePoolReleaseQueueNode(MessageQueueElement *node)
{
	ensureInitialized();
	if (node < &g_queueNodePool[0] || node >= &g_queueNodePool[MESSAGE_POOL_QUEUE_NODE_COUNT]) {
		return MESSAGE_POOL_NOT_OWNED;
	}
	uint16_t index = (uint16_t)(node - &g_queueNodePool[0]);
	MessagePoolHandle handle = makeHandle(MESSAGE_POOL_KIND_QUEUE_NODE, index, g_queueNodeState.generation[index]);
	uint8_t nested = enterCritical();
	MessagePoolStatus status = releaseIndex(&g_queueNodeState, handle);
	exitCritical(nested);
	return status;
}

static uint8_t *allocatePacketSlot(PoolState *state, PacketSlot *pool, size_t size, MessagePoolTrafficClass trafficClass)
{
	ensureInitialized();
	if (size > MESSAGE_POOL_PACKET_SLOT_SIZE) {
		messagePoolRecordOverflow();
		return NULL;
	}
	uint16_t index = 0;
	uint8_t nested = enterCritical();
	bool ok = allocateIndex(state, trafficClass, &index);
	exitCritical(nested);
	if (!ok) {
		messagePoolRecordOverflow();
		return NULL;
	}
	memset(pool[index].bytes, 0, MESSAGE_POOL_PACKET_SLOT_SIZE);
	return pool[index].bytes;
}

static MessagePoolStatus releasePacketSlot(PoolState *state, PacketSlot *pool, void *ptr, MessagePoolKind kind)
{
	ensureInitialized();
	MessagePoolHandle handle = handleForSlot(state, pool, ptr, kind);
	if (handle.index == UINT16_MAX) {
		return MESSAGE_POOL_NOT_OWNED;
	}
	uint8_t nested = enterCritical();
	MessagePoolStatus status = releaseIndex(state, handle);
	exitCritical(nested);
	return status;
}

uint8_t *messagePoolAllocatePacketBuffer(size_t size)
{
	return allocatePacketSlot(&g_packetBufferState, g_packetBufferPool, size, MESSAGE_POOL_TRAFFIC_STREAM_EVENT);
}

MessagePoolStatus messagePoolReleasePacketBuffer(uint8_t *buffer)
{
	return releasePacketSlot(&g_packetBufferState, g_packetBufferPool, buffer, MESSAGE_POOL_KIND_PACKET_BUFFER);
}

uint8_t *messagePoolAllocatePacketPayload(size_t size)
{
	return allocatePacketSlot(&g_packetPayloadState, g_packetPayloadPool, size, MESSAGE_POOL_TRAFFIC_STREAM_EVENT);
}

MessagePoolStatus messagePoolReleasePacketPayload(uint8_t *payload)
{
	return releasePacketSlot(&g_packetPayloadState, g_packetPayloadPool, payload, MESSAGE_POOL_KIND_PACKET_PAYLOAD);
}

bool messagePoolPacketSizeFits(size_t size)
{
	return size <= MESSAGE_POOL_PACKET_SLOT_SIZE;
}

uint32_t messagePoolOverflowCount(void)
{
	ensureInitialized();
	return g_overflowCount;
}

void messagePoolRecordOverflow(void)
{
	ensureInitialized();
	uint8_t nested = enterCritical();
	g_overflowCount++;
	exitCritical(nested);
}
