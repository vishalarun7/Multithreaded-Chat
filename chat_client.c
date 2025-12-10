#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "udp.h"

#define GLOBAL_LOG_FILE "global.txt"
#define ROOM_LOG_FILE "room.txt"
#define PRIV_LOG_FILE "priv.txt"

struct client_context {
    int sd;
    struct sockaddr_in server_addr;
    FILE *global_log_fd;
    FILE *room_log_fd;
    FILE *priv_log_fd;
    volatile sig_atomic_t running;
};

static int initialise_log_file(const char *path, FILE **fp, const char *label) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Failed to reset %s log (%s): %s\n", label, path, strerror(errno));
        return -1;
    }
    fclose(file);

    file = fopen(path, "a");
    if (!file) {
        fprintf(stderr, "Failed to open %s log (%s): %s\n", label, path, strerror(errno));
        return -1;
    }

    *fp = file;
    return 0;
}

static void trim_newline(char *line) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }
}

static int request_is_disconnect(const char *req) {
    return strncmp(req, "disconn$", 8) == 0;
}

static void *listener_thread(void *arg) {
    struct client_context *ctx = (struct client_context *)arg;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in responder;

    while (ctx->running) {
        int rc = udp_socket_read(ctx->sd, &responder, buffer, BUFFER_SIZE - 1);

        if (rc < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                usleep(10000);
                continue;
            }
            if (errno == EINTR) continue;

            fprintf(stderr, "listener: recv failed (%s)\n", strerror(errno));
            perror("udp_socket_read");
            ctx->running = 0;
            break;
        }

        buffer[rc] = '\0';

        size_t msg_len = strnlen(buffer, BUFFER_SIZE);
        if (msg_len == 0)
            continue;

        fwrite(buffer, 1, msg_len, ctx->global_log_fd);
        if (buffer[msg_len - 1] != '\n') {
            fputc('\n', ctx->global_log_fd);
        }
        fflush(ctx->global_log_fd);
    }

    return NULL;
}

static void *sender_thread(void *arg) {
    struct client_context *ctx = (struct client_context *)arg;
    char request[BUFFER_SIZE];

    while (ctx->running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(request, sizeof(request), stdin)) {
            ctx->running = 0;
            break;
        }

        trim_newline(request);

        if (request[0] == '\0')
            continue;

        int rc = udp_socket_write(ctx->sd, &ctx->server_addr, request, (int)strlen(request) + 1);
        if (rc < 0) {
            fprintf(stderr, "sender: send failed (%s)\n", strerror(errno));
            perror("udp_socket_write");
            ctx->running = 0;
            break;
        }

        if (request_is_disconnect(request)) {
            ctx->running = 0;
            break;
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    const char *server_ip = "127.0.0.1";
    int client_port = 0;

    if (argc > 1) {
        server_ip = argv[1];
    }
    if (argc > 2) {
        client_port = atoi(argv[2]);
        if (client_port < 0 || client_port > 65535) {
            fprintf(stderr, "Invalid client port: %d\n", client_port);
            return EXIT_FAILURE;
        }
    }

    int sd = udp_socket_open(client_port);
    if (sd < 0) {
        fprintf(stderr, "Failed to open UDP socket on port %d\n", client_port);
        perror("udp_socket_open");
        return EXIT_FAILURE;
    }

    // Make socket non-blocking to force listener thread to exit cleanly
    fcntl(sd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in server_addr;
    if (set_socket_addr(&server_addr, server_ip, SERVER_PORT) < 0) {
        fprintf(stderr, "Invalid server IP address: %s\n", server_ip);
        close(sd);
        return EXIT_FAILURE;
    }

    // initialize log files
    FILE *global_log_fd = NULL;
    FILE *room_log_fd = NULL;
    FILE *priv_log_fd = NULL;

    if (initialise_log_file(GLOBAL_LOG_FILE, &global_log_fd, "global") < 0) {
        close(sd);
        return EXIT_FAILURE;
    }

    if (initialise_log_file(ROOM_LOG_FILE, &room_log_fd, "room") < 0) {
        fclose(global_log_fd);
        close(sd);
        return EXIT_FAILURE;
    }

    if (initialise_log_file(PRIV_LOG_FILE, &priv_log_fd, "private") < 0) {
        fclose(global_log_fd);
        fclose(room_log_fd);
        close(sd);
        return EXIT_FAILURE;
    }

    struct client_context ctx = {
        .sd = sd,
        .server_addr = server_addr,
        .global_log_fd = global_log_fd,
        .room_log_fd = room_log_fd,
        .priv_log_fd = priv_log_fd,
        .running = 1
    };

    pthread_t sender, listener;
    pthread_create(&listener, NULL, listener_thread, &ctx);
    pthread_create(&sender, NULL, sender_thread, &ctx);

    pthread_join(sender, NULL);

    ctx.running = 0;

    pthread_join(listener, NULL);

    fclose(global_log_fd);
    fclose(room_log_fd);
    fclose(priv_log_fd);
    close(sd);
    return 0;
}
