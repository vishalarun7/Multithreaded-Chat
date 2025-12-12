#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include "circular_queue.h"
#include "activity_heap.h"
#define MAX_NAME_LEN 64
#define MAX_MUTED 16

struct client_node {
    char name[MAX_NAME_LEN];
    struct sockaddr_in addr;
    char muted[MAX_MUTED][MAX_NAME_LEN];
    int muted_count;
    time_t last_active;
    time_t last_ping_sent;
    int waiting_ping;
    int heap_index;
    struct client_node *next;
};

struct server_state {
    struct client_node *head;
    pthread_rwlock_t rwlock;
    message_queue msg_queue;
    struct activity_heap activity;
};

void init_server_state(struct server_state *s);
void destroy_server_state(struct server_state *s);

static void send_with_newline(int sd, const struct sockaddr_in *addr, const char *msg);
void say_message(struct server_state *s, int sd, const char *msg, const char *sender_name);
int say_to(struct server_state *s, int sd, const char *msg, const char *recipient_name, const char *sender_name);


struct client_node *find_client_by_name(struct server_state *s, const char *name);
struct client_node *find_client_by_addr(struct server_state *s, const struct sockaddr_in *addr);

int add_client(struct server_state *s,
               const struct sockaddr_in *addr,
               const char *name);

int remove_client_by_name(struct server_state *s,
                          const char *name);

int remove_client_by_addr(struct server_state *s,
                          const struct sockaddr_in *addr);

int rename_client(struct server_state *s,
                  const struct sockaddr_in *addr,
                  const char *newname);


int add_muted_for_client(struct server_state *s,
                         const char *requester,
                         const char *muted_name);

int remove_muted_for_client(struct server_state *s,
                            const char *requester,
                            const char *muted_name);

int is_muted_for_receiver(struct client_node *receiver,
                          const char *sender_name);

#endif // SERVER_H
