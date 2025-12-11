#include "udp.h"
#include "chat_server.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "circular_queue.h"

static void update_client_activity(struct server_state *state, const struct sockaddr_in *addr) {
    if (!state || !addr) return;
    pthread_rwlock_wrlock(&state->rwlock);
    struct client_node *cur = state->head;
    while (cur) {
        if (cur->addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            cur->addr.sin_port == addr->sin_port) {
            cur->last_active = time(NULL);
            activity_heap_update(&state->activity, cur);
            break;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&state->rwlock);
}

void init_server_state(struct server_state *s) {
    s->head = NULL;
    queue_init(&s->msg_queue);
    pthread_rwlock_init(&s->rwlock, NULL);
    activity_heap_init(&s->activity);
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
    //I allocate memory to store client_node and initialize its fields to 0 
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
    node->heap_index = -1;
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
    cur = s->head; //find oldname
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
    return -1; // client not found
}

int add_muted_for_client(struct server_state *s, const char *requester, const char *muted_name) {
    if (!requester || !muted_name) return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, requester, MAX_NAME_LEN) == 0) {
            // check if already muted
            for (int i = 0; i < cur->muted_count; ++i) {
                if (strncmp(cur->muted[i], muted_name, MAX_NAME_LEN) == 0) {
                    pthread_rwlock_unlock(&s->rwlock);
                    return -1; 
                }
            }
            if (cur->muted_count >= MAX_MUTED) {
                pthread_rwlock_unlock(&s->rwlock);
                return -1; //full
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

// this tests whether a receiver has muted a given sender_name 
int is_muted_for_receiver(struct client_node *receiver, const char *sender_name) {
    if (!receiver || !sender_name) return 0;
    for (int i = 0; i < receiver->muted_count; ++i) {
        if (strncmp(receiver->muted[i], sender_name, MAX_NAME_LEN) == 0) {
            return 1; 
        }
    }
    return 0;
}

static void send_with_newline(int sd, const struct sockaddr_in *addr, const char *msg) {
    char buffer[BUFFER_SIZE];
    size_t len = strnlen(msg, BUFFER_SIZE - 2);
    memcpy(buffer, msg, len);
    if (len == 0 || buffer[len - 1] != '\n') {
        buffer[len++] = '\n';
    }
    buffer[len] = '\0';
    if (udp_socket_write(sd, (struct sockaddr_in *)addr, buffer, (int)len + 1) < 0) {
        fprintf(stderr, "server send failed (%s)\n", strerror(errno));
        perror("udp_socket_write");
    }
}

void say_message(struct server_state *s, int sd, const char *msg, const char *sender_name) {
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (sender_name && is_muted_for_receiver(cur, sender_name)) {
            cur = cur->next;
            continue;
        }
        send_with_newline(sd, &cur->addr, msg);
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
            send_with_newline(sd, &cur->addr, msg);
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        cur=cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
    return -1;
}

//this is the request object sent to handler threads
struct request {
    int sd;                         // server socket descriptor (to send replies) 
    struct sockaddr_in src;         // source address of packet
    char buf[BUFFER_SIZE];          // copy of received packet (null-terminated)
    int len;                        // number of bytes received
    struct server_state *state;     
};

//to ensure buffer is null terminated 
static void ensure_null_terminated(char *buf, int n) {
    if (n < 0) return;
    if (n < BUFFER_SIZE) buf[n] = '\0';
    else buf[BUFFER_SIZE - 1] = '\0';
}

//to trim leading spaces
static char *skip_spaces(char *s){
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

//parses and handles commmand in request
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
    // conn$ client_name
    if (strcmp(cmd, "conn") == 0) {
        if (add_client(req->state, &req->src, args) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[Server] %s successfully connected", args);
            send_with_newline(req->sd, &req->src, msg);
            
            pthread_rwlock_rdlock(&req->state->rwlock);
            message_queue *q = &req->state->msg_queue;
            int idx = q->head;
            for (int i = 0; i < q->size; i++) {
                send_with_newline(req->sd, &req->src, q->messages[idx]);
                idx = (idx + 1) % 15;
            }
            pthread_rwlock_unlock(&req->state->rwlock);
            }
            return;
    }

    // say$ msg
    if (strcmp(cmd, "say") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        if (args[0] == '\0') return;
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "%s: %s", sender->name, args);
        pthread_rwlock_wrlock(&req->state->rwlock);
        enqueue(&req->state->msg_queue, msg);
        pthread_rwlock_unlock(&req->state->rwlock);
        say_message(req->state, req->sd, msg, sender->name);
        return;
    }

    // sayto$ recipient_name msg
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
        snprintf(formatted, sizeof(formatted), "%s: %s", sender->name, msg);
        say_to(req->state, req->sd, formatted, recipient, sender->name);
        return;
    }

    // disconn$
    if (strcmp(cmd, "disconn") == 0) {
        remove_client_by_addr(req->state, &req->src);
        char bye[] = "[Server] Disconnected. Bye!";
        send_with_newline(req->sd, &req->src, bye);
        return;
    }

    // mute$ client_name
    if (strcmp(cmd, "mute") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        add_muted_for_client(req->state, sender->name, args);
        return;
    }

    // unmute$ client_name
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

    // rename$ new_name
    if (strcmp(cmd, "rename") == 0) {
        struct client_node *sender = find_client_by_addr(req->state, &req->src);
        if (!sender) return;
        char old[MAX_NAME_LEN];
        strncpy(old, sender->name, MAX_NAME_LEN);
        if (rename_client(req->state, &req->src, args) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[Server] You are now known as %s", args);
            send_with_newline(req->sd, &req->src, msg);
        }
        return;
    }

    // kick$ client_name
    if (strcmp(cmd, "kick") == 0) {
        struct client_node *client = find_client_by_name(req->state, args);
        if (!client) return;
        if (ntohs(req->src.sin_port) != 6666) { // not admin
            char notify[256];
            snprintf(notify, sizeof(notify), "[Server] You are not an admin");
            send_with_newline(req->sd, &req->src, notify);
            return;
        }
        else{ // admin
            char notify[256];
            snprintf(notify, sizeof(notify), "[Server] You have been removed from the chat. disconn$ to close safely or conn$ <name> to join back");
            send_with_newline(req->sd, &client->addr, notify);
            remove_client_by_name(req->state, args);
            char bc[256];
            snprintf(bc, sizeof(bc), "[Server] %s has been removed from the chat", args);
            say_message(req->state, req->sd, bc, NULL);
            return;
        }
    }
}

struct listener_args {
    int sd;                     // server socket descriptor
    struct server_state *state; 
};

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
    pthread_create(&listener, NULL, listener_thread, &args);

    printf("Server running on port %d...\n", SERVER_PORT);

    pthread_join(listener, NULL);

    destroy_server_state(&state);
    return 0;
}
