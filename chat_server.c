#include "udp.h"
#include "chat_server.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include "circular_queue.h"

void init_server_state(struct server_state *s) {
    s->head = NULL;
    queue_init(&s->msg_queue);
    pthread_rwlock_init(&s->rwlock, NULL);

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
    node->next = s->head;
    s->head = node;
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

void say_message(struct server_state *s, int sd, const char *msg, const char *sender_name) {
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (sender_name && is_muted_for_receiver(cur, sender_name)) {
            cur = cur->next;
            continue;
        }
        udp_socket_write(sd, &cur->addr, (char *)msg, (int)strlen(msg) + 1);
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
            udp_socket_write(sd, &cur->addr, (char *)msg, (int)strlen(msg) + 1);
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

    // conn$ client_name
    if (strcmp(cmd, "conn") == 0) {
        if (add_client(req->state, &req->src, args) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Hi %s, you have successfully connected to the chat", args);
            udp_socket_write(req->sd, &req->src, msg, strlen(msg)+1);
            
            pthread_rwlock_rdlock(&req->state->rwlock);
            message_queue *q = &req->state->msg_queue;
            int idx = q->head;
            for (int i = 0; i < q->size; i++) {
                udp_socket_write(req->sd, &req->src, q->messages[idx], strlen(q->messages[idx]) + 1);
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
        char bye[] = "Disconnected. Bye!";
        udp_socket_write(req->sd, &req->src, bye, sizeof(bye));
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
            snprintf(msg, sizeof(msg),
                     "You are now known as %s", args);
            udp_socket_write(req->sd, &req->src, msg, strlen(msg)+1);
        }
        return;
    }

    // kick$ client_name
    if (strcmp(cmd, "kick") == 0) {
        struct client_node *client = find_client_by_name(req->state, args);
        if (!client) return;

        char notify[256];
        snprintf(notify, sizeof(notify),
                 "You have been removed from the chat");
        udp_socket_write(req->sd, &client->addr,notify, strlen(notify)+1);
        remove_client_by_name(req->state, args);
        char bc[256];
        snprintf(bc, sizeof(bc),
                 "%s has been removed from the chat", args);
        say_message(req->state, req->sd, bc, NULL);
        return;
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

