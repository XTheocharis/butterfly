#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H

#include "pb.h"
#include "whad.pb.h"
#include <stdint.h>

typedef struct MessageQueueElement MessageQueueElement;
typedef struct MessageQueueElement {
	Message *message;
	MessageQueueElement *nextElement;
	uint32_t sourceTimestamp;
} MessageQueueElement;

typedef struct MessageQueue {
	size_t size;
	MessageQueueElement *firstElement;
	MessageQueueElement *lastElement;
} MessageQueue;

#endif
