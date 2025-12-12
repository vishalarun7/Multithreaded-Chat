#include <stdlib.h>
#include <string.h>
#include "room.h"

// Basic djb2 hash to map room names into fixed buckets
static unsigned room_hash_name(const char *name) {
    unsigned long hash = 5381;
    if (!name) return 0;
    int c;
    while ((c = *name++)) {
        hash = ((hash << 5) + hash) + (unsigned char)c;
    }
    return (unsigned)(hash % ROOM_BUCKETS);
}

// Releases a room plus all membership links
static void free_room(struct chat_room *room) {
    if (!room) return;
    struct room_member *member = room->members;
    while (member) {
        struct room_member *next = member->next;
        free(member);
        member = next;
    }
    free(room);
}

// Initialize every bucket to NULL and set up the mutex.
void room_table_init(struct room_table *table) {
    if (!table) return;
    pthread_mutex_init(&table->lock, NULL);
    pthread_mutex_lock(&table->lock);
    for (int i = 0; i < ROOM_BUCKETS; ++i) {
        table->buckets[i] = NULL;
    }
    pthread_mutex_unlock(&table->lock);
}

// Free every room stored in the table.
void room_table_destroy(struct room_table *table) {
    if (!table) return;
    pthread_mutex_lock(&table->lock);
    for (int i = 0; i < ROOM_BUCKETS; ++i) {
        struct chat_room *room = table->buckets[i];
        while (room) {
            struct chat_room *next = room->next;
            free_room(room);
            room = next;
        }
        table->buckets[i] = NULL;
    }
    pthread_mutex_unlock(&table->lock);
    pthread_mutex_destroy(&table->lock);
}

// Locate a room by name in O(bucket length)
struct chat_room *room_table_find(struct room_table *table, const char *name) {
    if (!table || !name) return NULL;
    unsigned idx = room_hash_name(name);
    pthread_mutex_lock(&table->lock);
    struct chat_room *room = table->buckets[idx];
    while (room) {
        if (strncmp(room->name, name, MAX_NAME_LEN) == 0) {
            pthread_mutex_unlock(&table->lock);
            return room;
        }
        room = room->next;
    }
    pthread_mutex_unlock(&table->lock);
    return NULL;
}

// Create and insert a new room: fails if name already exists
struct chat_room *room_table_insert(struct room_table *table, const char *name) {
    if (!table || !name || name[0] == '\0') return NULL;
    pthread_mutex_lock(&table->lock);
    unsigned idx = room_hash_name(name);
    struct chat_room *cursor = table->buckets[idx];
    while (cursor) {
        if (strncmp(cursor->name, name, MAX_NAME_LEN) == 0) {
            pthread_mutex_unlock(&table->lock);
            return NULL;
        }
        cursor = cursor->next;
    }
    struct chat_room *room = calloc(1, sizeof(*room));
    if (!room) {
        pthread_mutex_unlock(&table->lock);
        return NULL;
    }
    strncpy(room->name, name, MAX_NAME_LEN - 1);
    room->name[MAX_NAME_LEN - 1] = '\0';
    queue_init(&room->history);
    room->next = table->buckets[idx]; // get head of bucket
    table->buckets[idx] = room;
    pthread_mutex_unlock(&table->lock);
    return room;
}

// Remove a room by name and free its resources
int room_table_remove(struct room_table *table, const char *name) {
    if (!table || !name) return -1;
    unsigned idx = room_hash_name(name);
    pthread_mutex_lock(&table->lock);
    struct chat_room **ind = &table->buckets[idx];
    while (*ind) {
        if (strncmp((*ind)->name, name, MAX_NAME_LEN) == 0) {
            struct chat_room *del = *ind;
            *ind = del->next;
            free_room(del);
            pthread_mutex_unlock(&table->lock);
            return 0;
        }
        ind = &(*ind)->next;
    }
    pthread_mutex_unlock(&table->lock);
    return -1;
}
