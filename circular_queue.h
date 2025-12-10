#ifndef CIRCULAR_QUEUE_H
#define CIRCULAR_QUEUE_H

#include <stdbool.h>

#define max_messages 15
#define BUFFER 1024  

typedef struct {
    char messages[max_messages][BUFFER];
    int head;       // index of the oldest message
    int tail;       // index where mesages are written
    int size;      
} message_queue;

void queue_init(message_queue *q);


void enqueue(message_queue *q, const char *msg);

#endif
