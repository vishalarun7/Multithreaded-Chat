#include "udp.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

void init_server_state(struct server_state *s) {
    s->head = NULL;
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

int rename_client(struct server_state *s, const char *oldname, const char *newname) {
    if (!oldname || !newname) return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, newname, MAX_NAME_LEN) == 0) {
            pthread_rwlock_unlock(&s->rwlock);
            return -1; //new name taken 
        }
        cur = cur->next;
    }

    //find oldname
    cur = s->head;
    while (cur) {
        if (strncmp(cur->name, oldname, MAX_NAME_LEN) == 0) {
            strncpy(cur->name, newname, MAX_NAME_LEN-1);
            cur->name[MAX_NAME_LEN-1] = '\0';
            pthread_rwlock_unlock(&s->rwlock);
            return 0;
        }
        cur = cur->next;
    }

    pthread_rwlock_unlock(&s->rwlock);
    return -1; //if oldname not found 
}

int add_muted_for_client(struct server_state *s, const char *requester, const char *muted_name) {
    if (!requester || !muted_name) return -1;
    pthread_rwlock_wrlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        if (strncmp(cur->name, requester, MAX_NAME_LEN) == 0) {
            /* check if already muted */
            for (int i = 0; i < cur->muted_count; ++i) {
                if (strncmp(cur->muted[i], muted_name, MAX_NAME_LEN) == 0) {
                    pthread_rwlock_unlock(&s->rwlock);
                    return -1; /* already muted */
                }
            }
            if (cur->muted_count >= MAX_MUTED) {
                pthread_rwlock_unlock(&s->rwlock);
                return -1; /* full */
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
    return -1; /* requester not found */
}

// broadcast message to all
void broadcast(struct server_state *s, int sd, const char *msg) {
    pthread_rwlock_rdlock(&s->rwlock);
    struct client_node *cur = s->head;
    while (cur) {
        udp_socket_write(sd, &cur->addr, (char*)msg, strlen(msg)+1);
        cur = cur->next;
    }
    pthread_rwlock_unlock(&s->rwlock);
}

int main() {
    struct server_state state;
    init_server_state(&state);

    int sd = udp_socket_open(SERVER_PORT);

    pthread_t listener;
    pthread_create(&listener, NULL, listener_thread, &some_args);

    pthread_join(listener, NULL);
    destroy_server_state(&state);
}


