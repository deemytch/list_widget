/* ovpn-widget: общие типы и интерфейсы модулей. */

#ifndef OVPN_WIDGET_H
#define OVPN_WIDGET_H

#include <glib.h>

/* Протокол. */
#define OVPN_START_BYTE 0xFF
#define OVPN_MAX_LINES  25
#define OVPN_MAX_CHARS  60
#define OVPN_MAX_BYTES  (OVPN_MAX_CHARS * 4) /* символ UTF-8 занимает до 4 байт */

/* Значение счётчика, пока по сокету ничего не пришло. */
#define OVPN_NO_DATA (-1)

/* ------------------------------------------------------------------ */
/* Сообщение                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    guint  count;
    gchar *lines[OVPN_MAX_LINES];
} OvpnMessage;

void ovpn_message_free (OvpnMessage *message);

/* ------------------------------------------------------------------ */
/* Настройки                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    gchar *socket_path;
    gint   timeout_ms;
    gchar *font;
} OvpnConfig;

gchar *ovpn_config_file    (void);
void   ovpn_config_load    (OvpnConfig *config);
void   ovpn_config_save    (const OvpnConfig *config);
void   ovpn_config_clear   (OvpnConfig *config);

/* ------------------------------------------------------------------ */
/* Поток чтения сокета                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    OVPN_EVENT_BUSY,    /* клиент подключился, идёт передача */
    OVPN_EVENT_IDLE,    /* передача завершена по любой причине */
    OVPN_EVENT_MESSAGE  /* принято корректное сообщение */
} OvpnEventKind;

typedef struct {
    OvpnEventKind  kind;
    OvpnMessage   *message; /* заполнено только для OVPN_EVENT_MESSAGE */
} OvpnEvent;

void ovpn_event_free (OvpnEvent *event);

typedef struct _OvpnReader OvpnReader;

OvpnReader *ovpn_reader_start    (const gchar *socket_path, gint timeout_ms);
void        ovpn_reader_stop     (OvpnReader  *reader);
gint        ovpn_reader_event_fd (OvpnReader  *reader);
OvpnEvent  *ovpn_reader_pop      (OvpnReader  *reader);

/* ------------------------------------------------------------------ */
/* Журнал                                                             */
/* ------------------------------------------------------------------ */

void ovpn_log_open (void);
void ovpn_fatal    (const gchar *format, ...) G_GNUC_PRINTF (1, 2) G_GNUC_NORETURN;

#endif /* OVPN_WIDGET_H */
