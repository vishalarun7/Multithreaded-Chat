#include "circular_queue.h"
#include <stdio.h>
#include <string.h> 

void queue_init(message_queue *q){
    q->head = 0;
    q->tail = 0; 
    q->size = 0;
}


void enqueue(message_queue *q, const char *msg){
    strncpy(q->messages[q->tail], msg, BUFFER - 1);
    q->messages[q->tail][BUFFER - 1] = '\0';
    q->tail = (q->tail+1) % 15;
    if (q->size < 15) {
        q->size++;
    } else {
        q->head = (q->head + 1) % 15;
    }
}
