#include "test_framework.h"

#include "messagePool.h"

static void pool_exhaustion_when_message_pool_is_full(void)
{
    Message *messages[MESSAGE_POOL_MESSAGE_COUNT];

    messagePoolReset();

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        messages[i] = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(messages[i] != NULL, "message slot should allocate before exhaustion");
    }

    TEST_ASSERT(messagePoolAllocateMessage(NULL) == NULL, "message pool should reject newest allocation when exhausted");
    TEST_ASSERT_EQ_INT(1, messagePoolOverflowCount());

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(messages[i]));
    }
}

static void queue_reserve_when_streams_fill_queue(void)
{
    MessageQueueElement *streamNodes[MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE];
    MessageQueueElement *responseNodes[MESSAGE_POOL_COMMAND_RESPONSE_RESERVE];

    messagePoolReset();

    for (int i = 0; i < MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        streamNodes[i] = messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_STREAM_EVENT, NULL);
        TEST_ASSERT(streamNodes[i] != NULL, "stream node should allocate while reserve remains");
    }

    TEST_ASSERT(messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_STREAM_EVENT, NULL) == NULL,
                "stream events should drop newest before consuming response reserve");

    for (int i = 0; i < MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        responseNodes[i] = messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL);
        TEST_ASSERT(responseNodes[i] != NULL, "command responses should use reserved queue nodes");
    }

    TEST_ASSERT(messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL) == NULL,
                "command responses should report overflow when reserve is fully exhausted");

    for (int i = 0; i < MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(streamNodes[i]));
    }
    for (int i = 0; i < MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(responseNodes[i]));
    }
}

static void stale_handles_when_slot_is_reused(void)
{
    MessagePoolHandle oldHandle;
    MessagePoolHandle newHandle;

    messagePoolReset();
    Message *message = messagePoolAllocateMessage(&oldHandle);
    TEST_ASSERT(message != NULL, "message allocation should provide a handle");

    TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(message));
    TEST_ASSERT_EQ_INT(MESSAGE_POOL_DOUBLE_FREE, messagePoolReleaseMessage(message));
    TEST_ASSERT_EQ_INT(MESSAGE_POOL_STALE_HANDLE, messagePoolReleaseMessageHandle(oldHandle));

    Message *reused = messagePoolAllocateMessage(&newHandle);
    TEST_ASSERT(reused != NULL, "released slot should be reusable");
    TEST_ASSERT(oldHandle.generation != newHandle.generation, "slot reuse should advance generation");
    TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(reused));
}

static void timestamp_and_domain_tags_survive_queue_delay(void)
{
    const int tags[5] = {
        Message_generic_tag,
        Message_discovery_tag,
        Message_ble_tag,
        Message_dot15d4_tag,
        Message_esb_tag,
    };
    MessageQueue queue = {0, NULL, NULL};

    messagePoolReset();

    for (int i = 0; i < 5; i++) {
        Message *message = messagePoolAllocateMessage(NULL);
        MessageQueueElement *node = messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL);
        TEST_ASSERT(message != NULL, "message should allocate for domain round trip");
        TEST_ASSERT(node != NULL, "queue node should allocate for domain round trip");
        message->which_msg = (pb_size_t)tags[i];
        node->message = message;
        node->sourceTimestamp = (uint32_t)(1000 + i);
        node->nextElement = NULL;
        if (queue.lastElement == NULL) {
            queue.firstElement = node;
            queue.lastElement = node;
        }
        else {
            queue.lastElement->nextElement = node;
            queue.lastElement = node;
        }
        queue.size++;
    }

    for (int i = 0; i < 5; i++) {
        MessageQueueElement *node = queue.firstElement;
        queue.firstElement = node->nextElement;
        queue.size--;
        TEST_ASSERT_EQ_INT(tags[i], node->message->which_msg);
        TEST_ASSERT_EQ_INT(1000 + i, node->sourceTimestamp);
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(node->message));
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(node));
    }
}

static void packet_pool_validates_maxlen(void)
{
    uint8_t *buffers[MESSAGE_POOL_PACKET_BUFFER_COUNT];

    messagePoolReset();

    TEST_ASSERT(messagePoolPacketSizeFits(MESSAGE_POOL_RADIO_MAX_PACKET_SIZE), "radio MAXLEN should fit packet slot");
    TEST_ASSERT(!messagePoolPacketSizeFits(MESSAGE_POOL_PACKET_SLOT_SIZE + 1), "packet slot should reject oversize buffers");

    for (int i = 0; i < MESSAGE_POOL_PACKET_BUFFER_COUNT; i++) {
        buffers[i] = messagePoolAllocatePacketBuffer(MESSAGE_POOL_RADIO_MAX_PACKET_SIZE);
        TEST_ASSERT(buffers[i] != NULL, "packet buffer should allocate before exhaustion");
    }

    TEST_ASSERT(messagePoolAllocatePacketBuffer(MESSAGE_POOL_RADIO_MAX_PACKET_SIZE) == NULL,
                "packet buffer pool should reject newest allocation when exhausted");

    for (int i = 0; i < MESSAGE_POOL_PACKET_BUFFER_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleasePacketBuffer(buffers[i]));
    }
}

static void dispatch_regression_allocates_and_frees_all_pools(void)
{
    messagePoolReset();

    Message *msg = messagePoolAllocateMessage(NULL);
    MessageQueueElement *node = messagePoolAllocateQueueNode(
        MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL);
    TEST_ASSERT(msg != NULL, "message slot for dispatch response");
    TEST_ASSERT(node != NULL, "queue node for dispatch response");

    node->message = msg;
    node->sourceTimestamp = 42;
    node->nextElement = NULL;

    msg->which_msg = Message_board_tag;

    TEST_ASSERT_EQ_INT(Message_board_tag, (int)node->message->which_msg);
    TEST_ASSERT_EQ_INT(42, (int)node->sourceTimestamp);

    TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msg));
    TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(node));
    TEST_ASSERT_EQ_INT(0, messagePoolOverflowCount());
}

static void full_pool_cycle_restores_to_full_capacity(void)
{
    messagePoolReset();

    Message *msgs[MESSAGE_POOL_MESSAGE_COUNT];
    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        msgs[i] = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(msgs[i] != NULL, "allocate before first exhaustion");
    }
    TEST_ASSERT(messagePoolAllocateMessage(NULL) == NULL,
                "pool exhausted on first cycle");

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msgs[i]));
    }

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        msgs[i] = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(msgs[i] != NULL, "allocate after full recycle");
    }
    TEST_ASSERT(messagePoolAllocateMessage(NULL) == NULL,
                "pool exhausted again after recycle");

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msgs[i]));
    }
}

static void thousand_dispatch_round_trips_return_to_baseline(void)
{
    messagePoolReset();

    for (int i = 0; i < 1000; i++) {
        Message *msg = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(msg != NULL, "message should allocate during round-trip");

        MessageQueueElement *node = messagePoolAllocateQueueNode(
            MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL);
        TEST_ASSERT(node != NULL, "queue node should allocate during round-trip");

        node->message = msg;
        node->nextElement = NULL;
        node->sourceTimestamp = (uint32_t)i;

        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msg));
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(node));
    }

    TEST_ASSERT_EQ_INT(0, messagePoolOverflowCount());

    Message *msgs[MESSAGE_POOL_MESSAGE_COUNT];
    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        msgs[i] = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(msgs[i] != NULL, "pool should be at full capacity after 1000 round-trips");
    }
    TEST_ASSERT(messagePoolAllocateMessage(NULL) == NULL,
                "pool should be cleanly exhausted after 1000 round-trips");
    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msgs[i]));
    }
}

static void exhaustion_preserves_command_response_reserve(void)
{
    messagePoolReset();

    Message *msgs[MESSAGE_POOL_MESSAGE_COUNT];
    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        msgs[i] = messagePoolAllocateMessage(NULL);
        TEST_ASSERT(msgs[i] != NULL, "message should allocate before exhaustion");
    }

    TEST_ASSERT(messagePoolAllocateMessage(NULL) == NULL,
                "message pool should report exhaustion");

    MessageQueueElement *streamNodes[MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE];
    for (int i = 0; i < MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        streamNodes[i] = messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_STREAM_EVENT, NULL);
        TEST_ASSERT(streamNodes[i] != NULL, "stream nodes should allocate while reserve remains");
    }

    TEST_ASSERT(messagePoolAllocateQueueNode(MESSAGE_POOL_TRAFFIC_STREAM_EVENT, NULL) == NULL,
                "stream events must drop newest before consuming command response reserve");

    for (int i = 0; i < MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        MessageQueueElement *node = messagePoolAllocateQueueNode(
            MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, NULL);
        TEST_ASSERT(node != NULL, "command response reserve must be preserved during exhaustion");
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(node));
    }

    for (int i = 0; i < MESSAGE_POOL_MESSAGE_COUNT; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseMessage(msgs[i]));
    }
    for (int i = 0; i < MESSAGE_POOL_QUEUE_NODE_COUNT - MESSAGE_POOL_COMMAND_RESPONSE_RESERVE; i++) {
        TEST_ASSERT_EQ_INT(MESSAGE_POOL_OK, messagePoolReleaseQueueNode(streamNodes[i]));
    }
}

int main(void)
{
    test_framework_init();
    RUN_TEST(pool_exhaustion_when_message_pool_is_full);
    RUN_TEST(queue_reserve_when_streams_fill_queue);
    RUN_TEST(stale_handles_when_slot_is_reused);
    RUN_TEST(timestamp_and_domain_tags_survive_queue_delay);
    RUN_TEST(packet_pool_validates_maxlen);
    RUN_TEST(dispatch_regression_allocates_and_frees_all_pools);
    RUN_TEST(full_pool_cycle_restores_to_full_capacity);
    RUN_TEST(thousand_dispatch_round_trips_return_to_baseline);
    RUN_TEST(exhaustion_preserves_command_response_reserve);
    return test_framework_finish();
}
