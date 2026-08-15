/*
 * Формат конфига (~/.config/ovpn-widget/config):
 *   строка, начинающаяся с ';', '#' или '%' — комментарий,
 *   пустые строки пропускаются,
 *   первая значащая строка — путь к сокету,
 *   вторая — таймаут чтения из сокета в миллисекундах,
 *   третья — описание шрифта в формате Pango ("Sans Bold 11").
 */

#include "ovpn-widget.h"
#include <glib/gstdio.h>
#include <string.h>

#define OVPN_DEFAULT_TIMEOUT 500
#define OVPN_DEFAULT_FONT "Sans Bold 11"

#define OVPN_MIN_TIMEOUT 50
#define OVPN_MAX_TIMEOUT 60000

static const gchar comment_marks[] = ";#%";

gchar * ovpn_config_file(void){
    return g_build_filename(g_get_user_config_dir(), "ovpn-widget", "config", NULL);
}

static gboolean is_meaningful(const gchar *line){
    if (line[0] == '\0') return FALSE;
    return strchr (comment_marks, line[0]) == NULL;
}

/* Возвращает до трёх значащих строк файла; вызывающий освобождает через g_strfreev. */
static gchar **read_meaningful_lines (const gchar *path){
    gchar       *content = NULL;
    gchar      **raw;
    GPtrArray   *kept;
    guint        index;

    if (!g_file_get_contents (path, &content, NULL, NULL)) return NULL;

    raw = g_strsplit (content, "\n", -1);
    g_free (content);

    kept = g_ptr_array_new ();
    for (index = 0; raw[index] != NULL && kept->len < 3; index++){
        gchar *line = g_strstrip (raw[index]);
        if (is_meaningful (line)) g_ptr_array_add (kept, g_strdup (line));
    }
    g_strfreev (raw);
    g_ptr_array_add (kept, NULL);
    return (gchar **) g_ptr_array_free (kept, FALSE);
}

static gint parse_timeout(const gchar *text){
    gint64 value = g_ascii_strtoll(text, NULL, 10);
    if (value < OVPN_MIN_TIMEOUT || value > OVPN_MAX_TIMEOUT) return OVPN_DEFAULT_TIMEOUT;
    return (gint) value;
}

void ovpn_config_load (OvpnConfig *config){
    gchar  *path;
    gchar **lines;

    // default socket path
    config->socket_path = g_build_filename(g_get_user_runtime_dir(), "ovpn-widget.sock", NULL);
    config->timeout_ms = OVPN_DEFAULT_TIMEOUT;
    config->font = g_strdup(OVPN_DEFAULT_FONT);

    path = ovpn_config_file();
    lines = read_meaningful_lines (path);
    g_free (path);

    if (lines == NULL) return;

    if (lines[0] != NULL){
        g_free (config->socket_path);
        config->socket_path = g_strdup (lines[0]);
        if (lines[1] != NULL) config->timeout_ms = parse_timeout (lines[1]);
        if (lines[1] != NULL && lines[2] != NULL){
            g_free (config->font);
            config->font = g_strdup (lines[2]);
    }}
    g_strfreev (lines);
}

void ovpn_config_save (const OvpnConfig *config){
    gchar *path;
    gchar *directory;
    gchar *text;

    path = ovpn_config_file ();
    directory = g_path_get_dirname (path);

    if (g_mkdir_with_parents (directory, 0700) != 0)
        ovpn_fatal ("не удалось создать каталог настроек %s", directory);

    text = g_strdup_printf ("# Настройки ovpn-widget.\n"
                            "# Комментарии начинаются с ';', '#' или '%%'.\n"
                            "# Путь к сокету:\n"
                            "%s\n"
                            "# Таймаут чтения из сокета, мс:\n"
                            "%d\n"
                            "# Шрифт:\n"
                            "%s\n",
                            config->socket_path, config->timeout_ms, config->font);

    /* g_file_set_contents пишет во временный файл и переименовывает его. */
    if (!g_file_set_contents (path, text, -1, NULL))
        ovpn_fatal ("не удалось записать файл настроек %s", path);

    g_free (text);
    g_free (directory);
    g_free (path);
}

void
ovpn_config_clear (OvpnConfig *config)
{
    g_clear_pointer (&config->socket_path, g_free);
    g_clear_pointer (&config->font, g_free);
}
