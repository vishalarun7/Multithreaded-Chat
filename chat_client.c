#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "udp.h"

#define CHAT_LOG_FILE "iChat.txt"

struct client_context {
    int sd;
    struct sockaddr_in server_addr;
    FILE *log_fp;
    volatile sig_atomic_t running;
};

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
            ctx->running = 0;
            break;
        }

        buffer[rc] = '\0';

        fprintf(ctx->log_fp, "%s", buffer);
        fflush(ctx->log_fp);

        printf("%s", buffer);
        fflush(stdout);
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

        int rc = udp_socket_write(ctx->sd, &ctx->server_addr, request,
                                  (int)strlen(request) + 1);
        if (rc < 0) {
            fprintf(stderr, "sender: send failed (%s)\n", strerror(errno));
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

    FILE *log_fp = fopen(CHAT_LOG_FILE, "a");
    if (!log_fp) {
        perror("Failed to open chat log");
        close(sd);
        return EXIT_FAILURE;
    }

    fprintf(log_fp, "\n--- New Session Started ---\n");
    fflush(log_fp);

    struct client_context ctx = {
        .sd = sd,
        .server_addr = server_addr,
        .log_fp = log_fp,
        .running = 1
    };

    pthread_t sender, listener;
    pthread_create(&listener, NULL, listener_thread, &ctx);
    pthread_create(&sender, NULL, sender_thread, &ctx);

    pthread_join(sender, NULL);

    ctx.running = 0;

    pthread_join(listener, NULL);

    fclose(log_fp);
    close(sd);
    return 0;
}
