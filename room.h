#ifndef ROOM_H
#define ROOM_H

#include "circular_queue.h"
#include <pthread.h>

#ifndef MAX_NAME_LEN
#define MAX_NAME_LEN 64
#endif

#ifndef ROOM_BUCKETS
#define ROOM_BUCKETS 32
#endif

struct client_node;

struct room_member {
    struct client_node *client;
    struct room_member *next;
};

struct chat_room {
    char name[MAX_NAME_LEN];
    message_queue history;
    struct room_member *members;
    struct chat_room *next;
};

struct room_table {
    pthread_mutex_t lock;
    struct chat_room *buckets[ROOM_BUCKETS];
};

void room_table_init(struct room_table *table);
void room_table_destroy(struct room_table *table);
struct chat_room *room_table_find(struct room_table *table, const char *name);
struct chat_room *room_table_insert(struct room_table *table, const char *name);
int room_table_remove(struct room_table *table, const char *name);

#endif // ROOM_H
