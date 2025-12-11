#ifndef ACTIVITY_HEAP_H
#define ACTIVITY_HEAP_H

#include <stddef.h>

struct client_node;

struct activity_heap {
    struct client_node **nodes;
    size_t size;
    size_t capacity;
};

void activity_heap_init(struct activity_heap *heap);
void activity_heap_destroy(struct activity_heap *heap);
int activity_heap_push(struct activity_heap *heap, struct client_node *client);
void activity_heap_remove(struct activity_heap *heap, struct client_node *client);
void activity_heap_update(struct activity_heap *heap, struct client_node *client);

#endif // ACTIVITY_HEAP_H
