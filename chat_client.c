#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include "udp.h"

#define GLOBAL_LOG_FILE "global.txt"
#define ROOM_LOG_FILE "room.txt"
#define PRIV_LOG_FILE "priv.txt"

struct ui_context {
    GtkWidget *window;
    GtkWidget *input;
    GtkWidget *global_view;
    GtkWidget *room_view;
    GtkWidget *priv_view;
    GtkTextBuffer *global_buffer;
    GtkTextBuffer *room_buffer;
    GtkTextBuffer *priv_buffer;
};

struct client_context {
    int sd;
    struct sockaddr_in server_addr;
    FILE *global_log_fd;
    FILE *room_log_fd;
    FILE *priv_log_fd;
    struct ui_context ui;
    volatile sig_atomic_t running;
};

static GAsyncQueue *send_queue;

struct append_request {
    GtkTextBuffer *buffer;
    GtkWidget *view;
    char *text;
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

static gboolean append_dispatch(gpointer data) {
    struct append_request *req = data;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(req->buffer, &iter);
    gtk_text_buffer_insert(req->buffer, &iter, req->text, -1);
    gtk_text_buffer_get_end_iter(req->buffer, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(req->view), &iter, 0.0, FALSE, 0.0, 0.0);
    g_free(req->text);
    g_free(req);
    return G_SOURCE_REMOVE;
}

static void schedule_append(struct ui_context *ui, GtkWidget *view, GtkTextBuffer *buffer, const char *msg) {
    if (!ui || !view || !buffer || !msg) return;
    struct append_request *req = g_new0(struct append_request, 1);
    req->buffer = buffer;
    req->view = view;
    size_t len = strlen(msg);
    if (len == 0 || msg[len - 1] != '\n')
        req->text = g_strdup_printf("%s\n", msg);
    else
        req->text = g_strdup(msg);
    g_idle_add(append_dispatch, req);
}

static gboolean quit_idle(gpointer data) {
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static GtkWidget *create_log_section(const char *title,
                                     GtkWidget **view_out,
                                     GtkTextBuffer **buffer_out) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(section), "log-section");

    GtkWidget *title_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_card), "log-title-box");
    GtkWidget *label = gtk_label_new(title);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "log-title");
    gtk_box_pack_start(GTK_BOX(title_card), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), title_card, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_style_context_add_class(gtk_widget_get_style_context(scrolled), "log-scrolled");
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(view), "log-text");
    gtk_container_add(GTK_CONTAINER(scrolled), view);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    gtk_box_pack_start(GTK_BOX(section), scrolled, TRUE, TRUE, 0);

    if (view_out) *view_out = view;
    if (buffer_out) *buffer_out = buffer;
    return section;
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    struct client_context *ctx = user_data;
    const char *text = gtk_entry_get_text(entry);
    if (!text) return;
    char *copy = g_strdup(text);
    g_strstrip(copy);
    if (copy[0] != '\0' && ctx && ctx->running) {
        g_async_queue_push(send_queue, copy);
    } else {
        g_free(copy);
    }
    gtk_entry_set_text(entry, "");
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    struct client_context *ctx = user_data;
    ctx->running = 0;
    if (send_queue) g_async_queue_push(send_queue, NULL);
    gtk_main_quit();
}

static void setup_ui(struct ui_context *ui, struct client_context *ctx) {
    memset(ui, 0, sizeof(*ui));
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "UDP Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 600);

    const char *css =
        "window { background:#030405; }"
        ".main-bg { background:#030405; padding:8px; }"
        ".log-section { background:#0b1118; border-radius:16px; padding:10px; border:1px solid #151c26; }"
        ".log-title-box { background:#101723; border-radius:12px; border:1px solid #1f2935; padding:6px; margin-bottom:6px; }"
        ".log-title { color:#f2f3f7; font-weight:bold; }"
        ".log-scrolled { background:#101723; border-radius:12px; padding:6px; border:1px solid #1f2935; }"
        ".log-text, .log-text text { background:#101723; color:#f8f8f8; }"
        ".chat-entry { background:#0c131d; color:#f8f8f8; border:1px solid #1a2230; padding:8px; border-radius:6px; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(main_box), "main-bg");
    gtk_container_add(GTK_CONTAINER(window), main_box);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(content_box), "main-bg");
    gtk_box_pack_start(GTK_BOX(main_box), content_box, TRUE, TRUE, 0);

    GtkWidget *global_section = create_log_section("Global", &ui->global_view, &ui->global_buffer);
    gtk_box_pack_start(GTK_BOX(content_box), global_section, TRUE, TRUE, 0);

    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(right_box), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_box), "main-bg");
    gtk_box_pack_start(GTK_BOX(content_box), right_box, TRUE, TRUE, 0);

    GtkWidget *room_section = create_log_section("Room", &ui->room_view, &ui->room_buffer);
    gtk_box_pack_start(GTK_BOX(right_box), room_section, TRUE, TRUE, 0);

    GtkWidget *priv_section = create_log_section("Private", &ui->priv_view, &ui->priv_buffer);
    gtk_box_pack_start(GTK_BOX(right_box), priv_section, TRUE, TRUE, 0);

    ui->input = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->input), "Enter command (conn$name, say$hi, etc.)");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->input), "chat-entry");
    gtk_box_pack_start(GTK_BOX(main_box), ui->input, FALSE, FALSE, 0);

    g_signal_connect(ui->input, "activate", G_CALLBACK(on_entry_activate), ctx);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), ctx);

    gtk_widget_show_all(window);
    ui->window = window;
}

static void schedule_quit(void) {
    g_idle_add(quit_idle, NULL);
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
            schedule_quit();
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

        schedule_append(&ctx->ui, ctx->ui.global_view, ctx->ui.global_buffer, buffer);
    }

    return NULL;
}

static void *sender_thread(void *arg) {
    struct client_context *ctx = (struct client_context *)arg;

    while (ctx->running) {
        char *request = g_async_queue_pop(send_queue);
        if (!request)
            break;

        trim_newline(request);
        if (request[0] == '\0') {
            g_free(request);
            continue;
        }

        int rc = udp_socket_write(ctx->sd, &ctx->server_addr, request, (int)strlen(request) + 1);
        if (rc < 0) {
            fprintf(stderr, "sender: send failed (%s)\n", strerror(errno));
            perror("udp_socket_write");
            g_free(request);
            ctx->running = 0;
            schedule_quit();
            break;
        }

        if (request_is_disconnect(request)) {
            g_free(request);
            ctx->running = 0;
            schedule_quit();
            break;
        }

        g_free(request);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

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

    fcntl(sd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in server_addr;
    if (set_socket_addr(&server_addr, server_ip, SERVER_PORT) < 0) {
        fprintf(stderr, "Invalid server IP address: %s\n", server_ip);
        close(sd);
        return EXIT_FAILURE;
    }

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

    send_queue = g_async_queue_new();
    setup_ui(&ctx.ui, &ctx);

    pthread_t listener, sender;
    pthread_create(&listener, NULL, listener_thread, &ctx);
    pthread_create(&sender, NULL, sender_thread, &ctx);

    gtk_main();

    ctx.running = 0;
    if (send_queue) g_async_queue_push(send_queue, NULL);

    pthread_join(sender, NULL);
    pthread_join(listener, NULL);

    if (send_queue) {
        g_async_queue_unref(send_queue);
        send_queue = NULL;
    }

    fclose(global_log_fd);
    fclose(room_log_fd);
    fclose(priv_log_fd);
    close(sd);
    return 0;
}
