#include "udp.h"
#include "chat_server.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "circular_queue.h"

#define MSG_GLOBAL 0x00
#define MSG_ROOM   0x01
#define MSG_PRIV   0x02

#define INACTIVITY_THRESHOLD 300
#define PING_TIMEOUT 10
#define PING_MONITOR_SLEEP_USEC 500000

struct listener_args {
    int sd;
    struct server_state *state;
};

static void update_client_activity(struct server_state *state, const struct sockaddr_in *addr) {
    if (!state || !addr) return;
    pthread_rwlock_wrlock(&state->rwlock);
    struct client_node *cur = state->head;
    while (cur) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr && cur->addr.sin_port == addr->sin_port) {
            cur->last_active = time(NULL);
            cur->waiting_ping = 0;
            activity_heap_update(&state->activity, cur);
            break;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&state->rwlock);
}

static int room_add_member(struct chat_room *room, struct client_node *client) {
    if (!room || !client) return -1;
    struct room_member *cur = room->members;
    while (cur) {
        if (cur->client == client) return 0;
        cur = cur->next;
    }
    struct room_member *node = malloc(sizeof(*node));
    if (!node) return -1;
    node->client = client;
    node->next = room->members;
    room->members = node;
    client->room = room;
    return 0;
}

static void room_remove_member(struct chat_room *room, struct client_node *client) {
    if (!room || !client) return;
    struct room_member **ind = &room->members;
    while (*ind) {
        if ((*ind)->client == client) {
            struct room_member *del = *ind;
            *ind = del->next;
            free(del);
            break;
        }
        ind = &(*ind)->next;
    }
}

static void detach_client_from_room(struct server_state *state, struct client_node *client) {
    if (!state || !client || !client->room) return;
    struct chat_room *room = client->room;
    room_remove_member(room, client);
    client->room = NULL;
    if (!room->members) {
        room_table_remove(&state->rooms, room->name);
    }
}

static void send_global(int sd, const struct sockaddr_in *addr, const char *msg) {
    if (!addr || !msg) return;
    char prefixed[BUFFER_SIZE];
    prefixed[0] = MSG_GLOBAL;
    size_t len = strnlen(msg, BUFFER_SIZE-2);
    memcpy(prefixed+1, msg, len);
    prefixed[len+1] = '\n';
    prefixed[len+2] = '\0';
    udp_socket_write(sd, (struct sockaddr_in*)addr, prefixed, (int)(len+2));
}

static void send_room(int sd, const struct sockaddr_in *addr, const char *msg) {
    if (!addr || !msg) return;
    char prefixed[BUFFER_SIZE];
    prefixed[0] = MSG_ROOM;
    size_t len = strnlen(msg, BUFFER_SIZE-2);
    memcpy(prefixed+1, msg, len);
    prefixed[len+1] = '\n';
    prefixed[len+2] = '\0';
    udp_socket_write(sd, (struct sockaddr_in*)addr, prefixed, (int)(len+2));
}

static void send_private(int sd, const struct sockaddr_in *addr, const char *msg) {
    if (!addr || !msg) return;
    char prefixed[BUFFER_SIZE];
    prefixed[0] = MSG_PRIV;
    size_t len = strnlen(msg, BUFFER_SIZE-2);
    memcpy(prefixed+1, msg, len);
    prefixed[len+1] = '\n';
    prefixed[len+2] = '\0';
    udp_socket_write(sd, (struct sockaddr_in*)addr, prefixed, (int)(len+2));
}

static void *ping_monitor_thread(void *arg) {
    struct listener_args *args = arg;
    int sd = args->sd;
    struct server_state *state = args->state;

    while (1) {
        int action = 0;
        struct sockaddr_in target_addr = {0};
        char target_name[MAX_NAME_LEN] = {0};
        useconds_t sleep_us = PING_MONITOR_SLEEP_USEC;

        pthread_rwlock_wrlock(&state->rwlock);
        struct client_node *oldest = activity_heap_peek(&state->activity);
        if (oldest) {
            time_t now = time(NULL);
            time_t idle = now - oldest->last_active;
            if (idle >= INACTIVITY_THRESHOLD) {
                if (!oldest->waiting_ping) {
                    oldest->waiting_ping = 1;
                    oldest->last_ping_sent = now;
                    target_addr = oldest->addr;
                    action = 1;
                    sleep_us = PING_MONITOR_SLEEP_USEC;
                } else if (now - oldest->last_ping_sent >= PING_TIMEOUT) {
                    target_addr = oldest->addr;
                    strncpy(target_name, oldest->name, MAX_NAME_LEN);
                    target_name[MAX_NAME_LEN - 1] = '\0';
                    action = 2;
                    sleep_us = PING_MONITOR_SLEEP_USEC;
                } else {
                    time_t wait = (oldest->last_ping_sent + PING_TIMEOUT) - now;
                    if (wait > 0) {
                        sleep_us = (useconds_t)(wait * 1000000L);
                    }
                }
            } else {
                time_t wait = INACTIVITY_THRESHOLD - idle;
                if (wait > 0){
                    sleep_us = (useconds_t)(wait * 1000000L);
                }
                else{
                    sleep_us = PING_MONITOR_SLEEP_USEC;
                }
            }
        } else {
            sleep_us = PING_MONITOR_SLEEP_USEC;
        }
        pthread_rwlock_unlock(&state->rwlock);

        if (action == 1) {
            send_global(sd, &target_addr, "ping$");
        } else if (action == 2) {
            char notify[256];
            snprintf(notify, sizeof(notify), "[Server] Disconnected due to inactivity. ");
            send_global(sd, &target_addr, notify);
            remove_client_by_addr(state, &target_addr);
            char bc[256];
            snprintf(bc, sizeof(bc), "[Server] %s was disconnected due to inactivity", target_name);
            say_message(state, sd, bc, NULL);
        }

        usleep(sleep_us);
    }

    return NULL;
}

void init_server_state(struct server_state *s) {
    s->head = NULL;
    queue_init(&s->msg_queue);
    pthread_rwlock_init(&s->rwlock, NULL);
    activity_heap_init(&s->activity);
    room_table_init(&s->rooms);
}

void destroy_server_state(struct server_state *s) {
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        struct client_node *next = cur->next;
        free(cur);
        cur = next;
    }
    s->head = NULL;
    pthread_rwlock_unlock(&s->rwlock);
    pthread_rwlock_destroy(&s->rwlock);
    activity_heap_destroy(&s->activity);
    room_table_destroy(&s->rooms);
}

struct client_node *find_client_by_name(struct server_state *s, const char *name) {
    struct client_node *result = NULL;
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, name, MAX_NAME_LEN) == 0) {
            result = cur;
            break;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return result;
}

struct client_node *find_client_by_addr(struct server_state *s, const struct sockaddr_in *addr){
    struct client_node *result = NULL;
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            cur->addr.sin_port == addr->sin_port) {
            result = cur;
            break;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return result;
}

int add_client(struct server_state *s, const struct sockaddr_in *addr, const char *name) {
    if (!name || name[0] == '\0') return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, name, MAX_NAME_LEN) == 0) {
            pthread_rwlock_unlock(&s->rwlock);
            return -1;
        }
        cur = cur->next;
    }
    struct client_node *node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_rwlock_unlock(&s->rwlock);
        return -1;
    }
    strncpy(node->name, name, MAX_NAME_LEN - 1);
    node->name[MAX_NAME_LEN - 1] = '\0';
    memcpy(&node->addr, addr, sizeof(*addr));
    node->muted_count = 0;
    node->last_active = time(NULL);
    node->last_ping_sent = 0;
    node->waiting_ping = 0;
    node->heap_index = -1;
    node->room = NULL;
    node->next = s->head;
    s->head = node;
    if (activity_heap_push(&s->activity, node) != 0) {
        s->head = node->next;
        free(node);
        pthread_rwlock_unlock(&s->rwlock);
        return -1;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return 0;
}

int remove_client_by_name(struct server_state *s, const char *name) {
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node **ind = &s->head;
    while (*ind) {
        if (strncmp((*ind)->name, name, MAX_NAME_LEN) == 0) {
            struct client_node *del = *ind;
            *ind = del->next;
            detach_client_from_room(s, del);
            activity_heap_remove(&s->activity, del);
            free(del);
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        ind = &(*ind)->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1;
}

int remove_client_by_addr(struct server_state *s, const struct sockaddr_in *addr) {
    if (!addr) return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node **ind = &s->head;
    while (*ind) {
        if ((*ind)->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            (*ind)->addr.sin_port == addr->sin_port)
        {
            struct client_node *del = *ind;
            *ind = del->next;
            detach_client_from_room(s, del);
            activity_heap_remove(&s->activity, del);
            free(del);
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        ind = &(*ind)->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1; 
}

int rename_client(struct server_state *s, const struct sockaddr_in *addr, const char *newname) {
    if (!addr || !newname || newname[0] == '\0') return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, newname, MAX_NAME_LEN) == 0) {
            pthread_rwlock_unlock(&s->rwlock);
            return -1;
        }
        cur = cur->next;
    }
    cur = s->head;
    while (cur) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            cur->addr.sin_port == addr->sin_port)
        {
            strncpy(cur->name, newname, MAX_NAME_LEN - 1);
            cur->name[MAX_NAME_LEN - 1] = '\0';
            pthread_rwlock_unlock(&s->rwlock);
            return 0; 
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1;
}

int add_muted_for_client(struct server_state *s, const char *requester, const char *muted_name) {
    if (!requester || !muted_name) return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, requester, MAX_NAME_LEN) == 0) {
            for (int i = 0; i < cur->muted_count; ++i) {
                if (strncmp(cur->muted[i], muted_name, MAX_NAME_LEN) == 0) {
                    pthread_rwlock_unlock(&s->rwlock);
                    return -1; 
                }
            }
            if (cur->muted_count >= MAX_MUTED) {
                pthread_rwlock_unlock(&s->rwlock);
                return -1;
            }
            strncpy(cur->muted[cur->muted_count], muted_name, MAX_NAME_LEN - 1);
            cur->muted[cur->muted_count][MAX_NAME_LEN - 1] = '\0';
            cur->muted_count++;
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1; 
}

int is_muted_for_receiver(struct client_node *receiver, const char *sender_name) {
    if (!receiver || !sender_name) return 0;
    for (int i = 0; i < receiver->muted_count; ++i) {
        if (strncmp(receiver->muted[i], sender_name, MAX_NAME_LEN) == 0) {
            return 1; 
        }
    }
    return 0;
}

void say_message(struct server_state *s, int sd, const char *msg, const char *sender_name) {
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (sender_name && is_muted_for_receiver(cur, sender_name)) {
            cur = cur->next;
            continue;
        }
        send_global(sd, &cur->addr, msg);
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
}

int say_to(struct server_state*s, int sd, const char *msg, const char *recipient_name, const char *sender_name){
    if (!recipient_name || !msg || !sender_name) return -1; 
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strcmp(cur->name, recipient_name) == 0){
            if (is_muted_for_receiver(cur, sender_name)) {
                pthread_rwlock_unlock(&s->rwlock);
                return 0; 
            }
            send_private(sd, &cur->addr, msg);
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        cur=cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1;
}

struct request {
    int sd;
    struct sockaddr_in src;
    char buf[BUFFER_SIZE];
    int len;
    struct server_state *state;
};

static void ensure_null_terminated(char *buf, int n) {
    if (n < 0) return;
    if (n < BUFFER_SIZE) buf[n] = '\0';
    else buf[BUFFER_SIZE - 1] = '\0';
}

static char *skip_spaces(char *s){
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void handle_request(struct request *req) {
    ensure_null_terminated(req->buf, req->len);
    char *p = skip_spaces(req->buf);
    char *dollar = strchr(p, '$');
    if (!dollar) return;
    *dollar = '\0';
    char *cmd = p;
    char *args = skip_spaces(dollar + 1);

    if (strcmp(cmd, "conn") != 0) {
        update_client_activity(req->state, &req->src);
    }
    if (strcmp(cmd, "conn") == 0) {
        if (add_client(req->state, &req->src, args) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[Server] %s successfully connected", args);
            send_global(req->sd, &req->src, msg);
            
            pthread_rwlock_rdlock(&req->state->rwlock);
            message_queue *q = &req->state->msg_queue;
            int idx = q->head;
            for (int i = 0; i < q->size; i++) {
                send_global(req->sd, &req->src, q->messages[idx]);
                idx = (idx + 1) % 15;
            }
            pthread_rwlock_unlock(&req->state->rwlock);
        }
        return;
    }

    if (strcmp(cmd, "re-ping") == 0) {
        return;
    }

    if (strcmp(cmd, "createroom") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        if (args[0] == '\0') {
            send_global(req->sd, &req->src, "[Server] Room name required");
            return;
        }
        pthread_rwlock_wrlock(&req->state->rwlock);
        if (sender->room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Leave your current room before creating a new one");
            return;
        }
        struct chat_room *room = room_table_insert(&req->state->rooms, args);
        if (!room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Unable to create room (maybe name already exists)");
            return;
        }
        if (room_add_member(room, sender) != 0) {
            room_table_remove(&req->state->rooms, args);
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Failed to join new room");
            return;
        }
        pthread_rwlock_unlock(&req->state->rwlock);
        char msg[256];
        snprintf(msg, sizeof(msg), "[Server] Room <%s> created; you joined it", room->name);
        send_global(req->sd, &req->src, msg);
        return;
    }

    if (strcmp(cmd, "joinroom") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        if (args[0] == '\0') {
            send_global(req->sd, &req->src, "[Server] Room name required");
            return;
        }
        pthread_rwlock_wrlock(&req->state->rwlock);
        struct chat_room *room = room_table_find(&req->state->rooms, args);
        if (!room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Room not found");
            return;
        }
        if (sender->room == room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] You are already in that room");
            return;
        }
        if (sender->room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Leave your current room before joining another");
            return;
        }
        if (room_add_member(room, sender) != 0) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Failed to join room");
            return;
        }

        message_queue *history = &room->history;
        int idx = history->head;
        for (int i = 0; i < history->size; ++i) {
            send_room(req->sd, &req->src, history->messages[idx]);
            idx = (idx + 1) % max_messages;
        }
        pthread_rwlock_unlock(&req->state->rwlock);
        char msg[256];
        snprintf(msg, sizeof(msg), "[Server] Joined room <%s>", room->name);
        send_global(req->sd, &req->src, msg);
        return;
    }

    if (strcmp(cmd, "sayroom") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;

        pthread_rwlock_wrlock(&req->state->rwlock);
        if (!sender->room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] You are not in a room");
            return;
        }
        if (args[0] == '\0') {
            pthread_rwlock_unlock(&req->state->rwlock);
            return;
        }
        char formatted[BUFFER_SIZE];
        snprintf(formatted, sizeof(formatted), "[%s|%s] %s",
                 sender->room->name, sender->name, args);

        enqueue(&sender->room->history, formatted);
        struct room_member *m = sender->room->members;
        while (m) {
            struct client_node *rc = m->client;
            if (rc && !is_muted_for_receiver(rc, sender->name)) {
                send_room(req->sd, &rc->addr, formatted);
            }
            m = m->next;
        }

        pthread_rwlock_unlock(&req->state->rwlock);
        return;
    }

    if (strcmp(cmd, "leaveroom") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        pthread_rwlock_wrlock(&req->state->rwlock);
        if (!sender->room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] You are not in a room");
            return;
        }
        char room_name[MAX_NAME_LEN];
        strncpy(room_name, sender->room->name, MAX_NAME_LEN - 1);
        room_name[MAX_NAME_LEN - 1] = '\0';
        detach_client_from_room(req->state, sender);
        pthread_rwlock_unlock(&req->state->rwlock);
        char msg[256];
        snprintf(msg, sizeof(msg), "[Server] You left room <%s>", room_name);
        send_global(req->sd, &req->src, msg);
        return;
    }

    if (strcmp(cmd, "kickroom") == 0) {
        if (ntohs(req->src.sin_port) != 6666) {
            send_global(req->sd, &req->src, "[Server] You are not an admin");
            return;
        }
        if (args[0] == '\0') {
            send_global(req->sd, &req->src, "[Server] Provide a client name to kick");
            return;
        }
        struct client_node *target = find_client_by_name(req->state, args);
        if (!target) {
            send_global(req->sd, &req->src, "[Server] Client not found");
            return;
        }
        pthread_rwlock_wrlock(&req->state->rwlock);
        if (!target->room) {
            pthread_rwlock_unlock(&req->state->rwlock);
            send_global(req->sd, &req->src, "[Server] Target is not in a room");
            return;
        }
        char room_name[MAX_NAME_LEN];
        strncpy(room_name, target->room->name, MAX_NAME_LEN - 1);
        room_name[MAX_NAME_LEN - 1] = '\0';
        struct sockaddr_in target_addr = target->addr;
        detach_client_from_room(req->state, target);
        pthread_rwlock_unlock(&req->state->rwlock);
        char notify[256];
        snprintf(notify, sizeof(notify), "[Server] You have been removed from room <%s>", room_name); 
        send_global(req->sd, &target_addr, notify);
        char ack[256];
        snprintf(ack, sizeof(ack), "[Server] %s removed from room <%s>", args, room_name);
        send_global(req->sd, &req->src, ack);
        return;
    }

    if (strcmp(cmd, "say") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        if (args[0] == '\0') return;
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "[%s] %s", sender->name, args);
        pthread_rwlock_wrlock(&req->state->rwlock);
        enqueue(&req->state->msg_queue, msg);
        pthread_rwlock_unlock(&req->state->rwlock);
        say_message(req->state, req->sd, msg, sender->name);
        return;
    }

    if (strcmp(cmd, "sayto") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        char recipient[MAX_NAME_LEN];
        char *space = strchr(args, ' ');
        if (!space) return;
        *space = '\0';
        strncpy(recipient, args, MAX_NAME_LEN);
        recipient[MAX_NAME_LEN-1] = '\0';
        char *msg = skip_spaces(space + 1);
        if (msg[0] == '\0') return;
        char formatted[BUFFER_SIZE];
        snprintf(formatted, sizeof(formatted), "[%s] %s", sender->name, msg);
        say_to(req->state, req->sd, formatted, recipient, sender->name);
        return;
    }

    if (strcmp(cmd, "disconn") == 0) {
        remove_client_by_addr(req->state, &req->src);
        char bye[] = "[Server] Disconnected. Bye!";
        send_global(req->sd, &req->src, bye);
        return;
    }

    if (strcmp(cmd, "mute") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        add_muted_for_client(req->state, sender->name, args);
        return;
    }

    if (strcmp(cmd, "unmute") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        pthread_rwlock_wrlock(&req->state->rwlock);
        for (int i = 0; i < sender->muted_count; i++) {
            if (strcmp(sender->muted[i], args) == 0) {
                for (int j = i; j < sender->muted_count-1; j++)
                    strncpy(sender->muted[j], sender->muted[j+1], MAX_NAME_LEN);
                sender->muted_count--;
                break;
            }
        }
        pthread_rwlock_unlock(&req->state->rwlock);
        return;
    }

    if (strcmp(cmd, "rename") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        char old[MAX_NAME_LEN];
        strncpy(old, sender->name, MAX_NAME_LEN);
        if (rename_client(req->state, &req->src, args) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[Server] You are now known as %s", args);
            send_global(req->sd, &req->src, msg);
        }
        return;
    }

    if (strcmp(cmd, "kick") == 0) {
        struct client_node *client = find_client_by_name(req->state, args);
        if (!client) return;
        if (ntohs(req->src.sin_port) != 6666) {
            char notify[256];
            snprintf(notify, sizeof(notify), "[Server] You are not an admin");
            send_global(req->sd, &req->src, notify);
            return;
        }
        else{
            char notify[256];
            snprintf(notify, sizeof(notify), "[Server] You have been removed from the chat. disconn$ to close safely or conn$ <name> to join back");
            send_global(req->sd, &client->addr, notify);
            remove_client_by_name(req->state, args);
            char bc[256];
            snprintf(bc, sizeof(bc), "[Server] %s has been removed from the chat", args);
            say_message(req->state, req->sd, bc, NULL);
            return;
        }
    }
}

void *request_handler_thread(void *arg) {
    if (!arg) return NULL;
    struct request *req = (struct request *)arg;
    handle_request(req);
    free(req);
    return NULL;
}

void *listener_thread(void *arg) {
    struct listener_args *args = arg;
    int sd = args->sd;
    struct server_state *state = args->state;

    while (1) {
        struct request *req = malloc(sizeof(struct request));
        if (!req) continue;

        req->sd = sd;
        req->state = state;

        socklen_t srclen = sizeof(req->src);
        req->len = udp_socket_read(sd, &req->src, req->buf, BUFFER_SIZE);
        if (req->len < 0) {
            perror("udp_socket_read");
            free(req);
            continue;
        }
        pthread_t worker;
        pthread_create(&worker, NULL, request_handler_thread, req);
        pthread_detach(worker);
    }
    return NULL;
}

int main() {
    struct server_state state;
    init_server_state(&state);

    int sd = udp_socket_open(SERVER_PORT);
    if (sd < 0) {
        fprintf(stderr, "Server failed to open UDP socket on port %d\n", SERVER_PORT);
        perror("udp_socket_open");
        destroy_server_state(&state);
        return 1;
    }

    struct listener_args args;
    args.sd = sd;
    args.state = &state;

    pthread_t listener;
    pthread_t pinger;
    pthread_create(&listener, NULL, listener_thread, &args);
    pthread_create(&pinger, NULL, ping_monitor_thread, &args);

    printf("Server running on port %d...\n", SERVER_PORT);

    pthread_join(listener, NULL);
    pthread_join(pinger, NULL);

    destroy_server_state(&state);
    close(sd);
    return 0;
}
