#include <stdlib.h>
#include "activity_heap.h"
#include "chat_server.h"

// Swaps two entries and keeps their stored heap indices consistent.
static void activity_heap_swap(struct activity_heap *heap, size_t a, size_t b) {
    struct client_node *tmp = heap->nodes[a];
    heap->nodes[a] = heap->nodes[b];
    heap->nodes[b] = tmp;
    heap->nodes[a]->heap_index = (int)a;
    heap->nodes[b]->heap_index = (int)b;
}

// Bubble the node at idx toward the root until the min-heap property holds
static void activity_heap_heapify_up(struct activity_heap *heap, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (heap->nodes[parent]->last_active <= heap->nodes[idx]->last_active)
            break;
        activity_heap_swap(heap, parent, idx);
        idx = parent;
    }
}

// Push the node at idx down the tree until both children are newer (larger timestamps)
static void activity_heap_heapify_down(struct activity_heap *heap, size_t idx) {
    while (1) {
        size_t left = idx * 2 + 1;
        size_t right = left + 1;
        size_t smallest = idx;
        if (left < heap->size && heap->nodes[left]->last_active < heap->nodes[smallest]->last_active)
            smallest = left;
        if (right < heap->size && heap->nodes[right]->last_active < heap->nodes[smallest]->last_active)
            smallest = right;
        if (smallest == idx) break;
        activity_heap_swap(heap, idx, smallest);
        idx = smallest;
    }
}

// Ensures the backing array can hold at least <needed> nodes
static int activity_heap_reserve(struct activity_heap *heap, size_t needed) {
    if (heap->capacity >= needed) return 0;
    size_t newcap = heap->capacity ? heap->capacity * 2 : 16;
    if (newcap < needed) newcap = needed;
    struct client_node **tmp = realloc(heap->nodes, newcap * sizeof(*tmp));
    if (!tmp) return -1;
    heap->nodes = tmp;
    heap->capacity = newcap;
    return 0;
}

// Initializes an empty min-heap
void activity_heap_init(struct activity_heap *heap) {
    if (!heap) return;
    heap->nodes = NULL;
    heap->size = 0;
    heap->capacity = 0;
}

// Releases heap storage; does not free client nodes themselves
void activity_heap_destroy(struct activity_heap *heap) {
    if (!heap) return;
    free(heap->nodes);
    heap->nodes = NULL;
    heap->size = 0;
    heap->capacity = 0;
}

// Inserts a client into the heap ordered by <last_active>
int activity_heap_push(struct activity_heap *heap, struct client_node *client) {
    if (!heap || !client) return -1;
    if (activity_heap_reserve(heap, heap->size + 1) != 0)
        return -1;
    heap->nodes[heap->size] = client;
    client->heap_index = (int)heap->size;
    heap->size++;
    activity_heap_heapify_up(heap, heap->size - 1);
    return 0;
}

// Removes an arbitrary client from the heap by index
void activity_heap_remove(struct activity_heap *heap, struct client_node *client) {
    if (!heap || !client) return;
    int idx = client->heap_index;
    if (idx < 0 || (size_t)idx >= heap->size) return;
    size_t last = heap->size - 1;
    if ((size_t)idx != last) {
        activity_heap_swap(heap, (size_t)idx, last);
    }
    heap->size--;
    if ((size_t)idx < heap->size) {
        activity_heap_heapify_down(heap, (size_t)idx);
        activity_heap_heapify_up(heap, (size_t)idx);
    }
    client->heap_index = -1;
}

// Reorders the heap after a client's timestamp changes
void activity_heap_update(struct activity_heap *heap, struct client_node *client) {
    if (!heap || !client) return;
    int idx = client->heap_index;
    if (idx < 0 || (size_t)idx >= heap->size) return;
    activity_heap_heapify_down(heap, (size_t)idx);
    activity_heap_heapify_up(heap, (size_t)idx);
}
