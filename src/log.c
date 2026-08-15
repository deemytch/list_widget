/* Сообщения об ошибках уходят в syslog по-русски, после чего процесс падает. */

#include "ovpn-widget.h"

#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

void ovpn_log_open (void){
    openlog ("ovpn-widget", LOG_PID, LOG_USER);
}

void ovpn_fatal (const gchar *format, ...){
    va_list  arguments;
    gchar   *text;

    va_start (arguments, format);
    text = g_strdup_vprintf (format, arguments);
    va_end (arguments);

    syslog (LOG_ERR, "%s", text);
    g_free (text);
    closelog ();

    /* Вызов возможен из потока чтения, поэтому уходим без обработчиков atexit. */
    _exit (EXIT_FAILURE);
}
