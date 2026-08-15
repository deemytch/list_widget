/*
 * Второй поток. Он единолично владеет unix-сокетом: создаёт файл, слушает,
 * принимает клиента, разбирает передачу и закрывает соединение.
 *
 * Наружу поток отдаёт события через очередь; чтобы разбудить главный поток,
 * в канал event_pipe пишется байт, который главный поток ждёт средствами GLib.
 * Обратно, из главного потока в этот, идёт единственный сигнал — «пора выйти»
 * через канал quit_pipe. Общих изменяемых данных нет, поэтому нет и блокировок.
 */

#include "ovpn-widget.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef enum {
    OVPN_OK,    /* всё прочитано */
    OVPN_ERROR, /* таймаут, обрыв или неверные данные */
    OVPN_QUIT   /* главный поток просит завершиться */
} OvpnResult;

struct _OvpnReader {
    gchar       *socket_path;
    gint         timeout_ms;
    GThread     *thread;
    GAsyncQueue *events;
    gint         quit_pipe[2];  /* [0] читает поток, [1] пишет главный */
    gint         event_pipe[2]; /* [0] читает главный, [1] пишет поток */
};

/* ------------------------------------------------------------------ */
/* Освобождение памяти                                                */
/* ------------------------------------------------------------------ */

void ovpn_message_free(OvpnMessage *message){
    guint index;
    if(message == NULL) return;
    for(index = 0; index < OVPN_MAX_LINES; index++) g_free(message->lines[index]);
    g_free(message);
}

void ovpn_event_free(OvpnEvent *event){
    if(event == NULL) return;
    ovpn_message_free(event->message);
    g_free(event);
}

/* ------------------------------------------------------------------ */
/* Отправка событий главному потоку                                   */
/* ------------------------------------------------------------------ */

static void post_event(OvpnReader *reader, OvpnEventKind kind, OvpnMessage *message){
    OvpnEvent   *event = g_new0(OvpnEvent, 1);
    const guint8 wakeup = 1;
    event->kind = kind;
    event->message = message;
    g_async_queue_push(reader->events, event);
    /* Канал неблокирующий: если он переполнен, главный поток и так не успевает
     * разгребать очередь и заберёт это событие вместе со следующим. */
    if(write(reader->event_pipe[1], &wakeup, 1) < 0) return;
}

/* ------------------------------------------------------------------ */
/* Ожидание и чтение                                                  */
/* ------------------------------------------------------------------ */

static OvpnResult wait_input(OvpnReader *reader, gint fd, gint timeout_ms){
    struct pollfd watch[2];
    memset(watch, 0, sizeof watch);
    watch[0].fd = fd;
    watch[0].events = POLLIN;
    watch[1].fd = reader->quit_pipe[0];
    watch[1].events = POLLIN;
    while(TRUE){
        gint ready = poll(watch, 2, timeout_ms);
        if(ready < 0 && errno == EINTR) continue;
        if(ready <= 0) return OVPN_ERROR; /* таймаут или сбой poll */
        if(watch[1].revents != 0) return OVPN_QUIT;
        return OVPN_OK;
}}

static OvpnResult read_exactly(OvpnReader *reader, gint fd, void *buffer, gsize size){
    guint8 *bytes = buffer;
    gsize   done = 0;
    while(done < size){
        OvpnResult result = wait_input(reader, fd, reader->timeout_ms);
        gssize     got;
        if(result != OVPN_OK) return result;
        got = read(fd, bytes + done, size - done);
        if(got > 0){
            done +=(gsize) got;
            continue;
        }
        if(got < 0 && errno == EINTR) continue;
        return OVPN_ERROR; /* клиент отключился посреди передачи */
    }
    return OVPN_OK;
}

/* ------------------------------------------------------------------ */
/* Разбор передачи                                                    */
/* ------------------------------------------------------------------ */

static OvpnResult read_line(OvpnReader *reader, gint fd, gchar **line){
    guint16    length;
    gchar      buffer[OVPN_MAX_BYTES];
    OvpnResult result;
    result = read_exactly (reader, fd, &length, 1);
    if (result != OVPN_OK) return result;
    if (length > OVPN_MAX_BYTES) return OVPN_ERROR;
    result = read_exactly (reader, fd, buffer, length);
    if (result != OVPN_OK) return result;
    if (!g_utf8_validate (buffer, length, NULL)) return OVPN_ERROR;
    if (g_utf8_strlen(buffer, length) > OVPN_MAX_CHARS) return OVPN_ERROR;
    *line = g_strndup(buffer, length);
    return OVPN_OK;
}

/* После двух нулевых байт клиент обязан закрыть соединение. */
static OvpnResult read_disconnect(OvpnReader *reader, gint fd){
    guint8     extra;
    OvpnResult result = wait_input(reader, fd, reader->timeout_ms);

    if (result != OVPN_OK)
        return result;

    if (read (fd, &extra, 1) != 0)
        return OVPN_ERROR; /* лишние данные вместо закрытия */

    return OVPN_OK;
}

static OvpnResult read_body(OvpnReader *reader, gint fd, OvpnMessage *message){
    guint8     header[2];
    guint8     tail[2];
    guint      index;
    OvpnResult result;

    result = read_exactly(reader, fd, header, sizeof header);
    if (result != OVPN_OK) return result;
    if (header[0] != OVPN_START_BYTE) return OVPN_ERROR;
    if (header[1] > OVPN_MAX_LINES) return OVPN_ERROR;
    message->count = header[1];
    for (index = 0; index < message->count; index++){
        result = read_line(reader, fd, &message->lines[index]);
        if (result != OVPN_OK) return result;
    }
    result = read_exactly(reader, fd, tail, sizeof tail);
    if (result != OVPN_OK) return result;
    if (tail[0] != 0 || tail[1] != 0) return OVPN_ERROR;
    return read_disconnect(reader, fd);
}

/* Сообщение отдаётся наружу только целиком и только после корректного конца. */
static OvpnResult receive_message (OvpnReader *reader, gint fd, OvpnMessage **received){
    OvpnMessage *message = g_new0 (OvpnMessage, 1);
    OvpnResult   result = read_body (reader, fd, message);
    if (result == OVPN_OK) *received = message;
    else ovpn_message_free(message);
    return result;
}

/* ------------------------------------------------------------------ */
/* Слушающий сокет                                                    */
/* ------------------------------------------------------------------ */

static gint listener_create(const gchar *path){
    struct sockaddr_un address;
    gchar             *directory;
    gint               fd;
    if (strlen (path) >= sizeof address.sun_path)
        ovpn_fatal ("путь к сокету длиннее %zu байт: %s", sizeof address.sun_path - 1, path);

    directory = g_path_get_dirname (path);
    g_mkdir_with_parents (directory, 0700);
    g_free (directory);
    unlink (path); /* убираем файл, оставшийся от прошлого запуска */
    fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) ovpn_fatal ("не удалось создать unix-сокет: %s", g_strerror (errno));
    memset (&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strcpy (address.sun_path, path); /* длина проверена выше */

    if (bind (fd, (struct sockaddr *) &address, sizeof address) < 0)
        ovpn_fatal ("не удалось привязать сокет %s: %s", path, g_strerror (errno));

    if (listen (fd, 1) < 0)
        ovpn_fatal ("не удалось начать приём на сокете %s: %s", path, g_strerror (errno));

    chmod (path, S_IRUSR | S_IWUSR);
    return fd;
}

static OvpnResult accept_client (OvpnReader *reader, gint listen_fd, gint *client_fd){
    OvpnResult result;
    gint       fd;
    /* Клиент подключается когда угодно, поэтому ждём без таймаута. */
    result = wait_input(reader, listen_fd, -1);
    if (result != OVPN_OK) return result;
    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return OVPN_ERROR;
    *client_fd = fd;
    return OVPN_OK;
}

/* ------------------------------------------------------------------ */
/* Главный цикл потока                                                */
/* ------------------------------------------------------------------ */

/* Обслуживает клиентов, пока не случится ошибка или не попросят выйти. */
static OvpnResult serve_clients(OvpnReader *reader, gint listen_fd){
    while (TRUE){
        OvpnMessage *message = NULL;
        OvpnResult   result;
        gint         client_fd = -1;
        result = accept_client(reader, listen_fd, &client_fd);
        if (result != OVPN_OK) return result;
        post_event (reader, OVPN_EVENT_BUSY, NULL);
        result = receive_message (reader, client_fd, &message);
        close (client_fd);
        if (result == OVPN_OK) post_event (reader, OVPN_EVENT_MESSAGE, message);
        post_event (reader, OVPN_EVENT_IDLE, NULL);
        if (result != OVPN_OK) return result;
}}

static gpointer reader_main(gpointer data){
    OvpnReader *reader = data;
    while (TRUE){
        gint listen_fd = listener_create(reader->socket_path);
        OvpnResult result = serve_clients(reader, listen_fd);
        /* После любой ошибки сокет пересоздаётся с нуля. */
        // listener_destroy (listen_fd, reader->socket_path);
        close(listen_fd);
        unlink(reader->socket_path);
        if (result == OVPN_QUIT) break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Интерфейс для главного потока                                      */
/* ------------------------------------------------------------------ */

static void open_pipe(gint fds[2], const gchar *name){
    if (pipe (fds) != 0)
        ovpn_fatal ("не удалось создать канал %s: %s", name, g_strerror (errno));

    fcntl (fds[0], F_SETFL, O_NONBLOCK);
    fcntl (fds[1], F_SETFL, O_NONBLOCK);
}

OvpnReader *ovpn_reader_start(const gchar *socket_path, gint timeout_ms)
{
    OvpnReader *reader = g_new0 (OvpnReader, 1);

    reader->socket_path = g_strdup (socket_path);
    reader->timeout_ms = timeout_ms;
    reader->events = g_async_queue_new ();
    open_pipe (reader->quit_pipe, "остановки");
    open_pipe (reader->event_pipe, "событий");

    reader->thread = g_thread_new ("ovpn-reader", reader_main, reader);
    return reader;
}

void ovpn_reader_stop(OvpnReader *reader){
    const guint8 signal_byte = 1;
    OvpnEvent   *event;

    if (write (reader->quit_pipe[1], &signal_byte, 1) < 0)
        ovpn_fatal ("не удалось остановить поток чтения: %s", g_strerror (errno));

    g_thread_join (reader->thread);

    while ((event = g_async_queue_try_pop (reader->events)) != NULL)
        ovpn_event_free (event);
    g_async_queue_unref (reader->events);

    close (reader->quit_pipe[0]);
    close (reader->quit_pipe[1]);
    close (reader->event_pipe[0]);
    close (reader->event_pipe[1]);

    g_free (reader->socket_path);
    g_free (reader);
}

gint ovpn_reader_event_fd(OvpnReader *reader){
    return reader->event_pipe[0];
}

OvpnEvent *ovpn_reader_pop(OvpnReader *reader){
    return g_async_queue_try_pop (reader->events);
}
