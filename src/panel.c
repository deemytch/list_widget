/*
 * Главный поток. Здесь только GTK: ни одного блокирующего вызова, вся работа
 * с сокетом вынесена в поток чтения (reader.c). События от него приходят
 * через канал, который отслеживает главный цикл GLib.
 */

#include "ovpn-widget.h"
#include <libxfce4panel/libxfce4panel.h>
#include <glib-unix.h>
#include <unistd.h>

/* Цвет счётчика на время передачи: тёмный серо-зелёный, независимо от темы. */
#define OVPN_BUSY_RED   0x4A4A
#define OVPN_BUSY_GREEN 0x5D5D
#define OVPN_BUSY_BLUE  0x4A4A

#define OVPN_FRAME_CSS \
    "#ovpn-frame {" \
    "  padding: 1px 4px;" \
    "  border: 1px solid rgba(128, 128, 128, 0.55);" \
    "  border-radius: 3px;" \
    "}"

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *frame;
    GtkWidget       *label;
    OvpnConfig       config;
    OvpnReader      *reader;
    guint            event_source;
    gboolean         busy;
    gint             count;   /* OVPN_NO_DATA, пока ничего не приходило */
    gchar           *tooltip; /* NULL, если показывать нечего */
} OvpnPanel;

static void ovpn_construct(XfcePanelPlugin *plugin);

XFCE_PANEL_PLUGIN_REGISTER(ovpn_construct)

/* ------------------------------------------------------------------ */
/* Отрисовка                                                          */
/* ------------------------------------------------------------------ */

static PangoAttrList * build_attributes(OvpnPanel *panel){
    PangoFontDescription *font;
    PangoAttrList        *attributes;

    font = pango_font_description_from_string(panel->config.font);
    if(font == NULL) ovpn_fatal("не удалось разобрать описание шрифта: %s", panel->config.font);

    attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_font_desc_new(font));

    if(panel->busy)
        pango_attr_list_insert(attributes,
                                pango_attr_foreground_new(OVPN_BUSY_RED,
                                                         OVPN_BUSY_GREEN,
                                                         OVPN_BUSY_BLUE));
    pango_font_description_free(font);
    return attributes;
}

static void refresh_widget(OvpnPanel *panel){
    PangoAttrList *attributes;
    gchar          text[8];
    gboolean       show_tooltip;

    g_snprintf(text, sizeof text, "%d", panel->count);
    gtk_label_set_text(GTK_LABEL(panel->label), text);

    attributes = build_attributes(panel);
    gtk_label_set_attributes(GTK_LABEL(panel->label), attributes);
    pango_attr_list_unref(attributes);

    /* Во время передачи подробности не показываем: данные ещё не приняты. */
    show_tooltip = !panel->busy && panel->tooltip != NULL;
    gtk_widget_set_tooltip_text(panel->frame, show_tooltip ? panel->tooltip : NULL);
    gtk_widget_set_has_tooltip(panel->frame, show_tooltip);
}

static void store_message(OvpnPanel *panel, const OvpnMessage *message){
    gchar *lines[OVPN_MAX_LINES + 1];
    guint  index;
    panel->count = (gint)message->count;
    g_clear_pointer(&panel->tooltip, g_free);
    if(message->count == 0) return;
    for(index = 0; index < message->count; index++) lines[index] = message->lines[index];
    lines[message->count] = NULL;
    panel->tooltip = g_strjoinv("\n", lines);
}

/* ------------------------------------------------------------------ */
/* События от потока чтения                                           */
/* ------------------------------------------------------------------ */

static void apply_event(OvpnPanel *panel, const OvpnEvent *event){
    switch(event->kind){
    case OVPN_EVENT_BUSY:
        panel->busy = TRUE;
        break;

    case OVPN_EVENT_IDLE:
        panel->busy = FALSE;
        break;

    case OVPN_EVENT_MESSAGE:
        store_message(panel, event->message);
        break;
}}

static gboolean on_reader_event(gint fd, GIOCondition condition, gpointer data){
    OvpnPanel *panel = data;
    OvpnEvent *event;
    guint8 wakeups[64];

    (void)condition;
    while(read(fd, wakeups, sizeof wakeups) > 0); /* канал нужен только чтобы разбудить главный цикл */
    while((event = ovpn_reader_pop(panel->reader)) != NULL){
        apply_event(panel, event);
        ovpn_event_free (event);
    }
    refresh_widget(panel);
    return G_SOURCE_CONTINUE;
}

static void reader_restart (OvpnPanel *panel){
    if(panel->reader != NULL){
      g_source_remove (panel->event_source);
      ovpn_reader_stop (panel->reader);
    }
    panel->busy = FALSE;
    panel->reader = ovpn_reader_start(panel->config.socket_path, panel->config.timeout_ms);
    panel->event_source = g_unix_fd_add(ovpn_reader_event_fd (panel->reader), G_IO_IN, on_reader_event, panel);
}

/* ------------------------------------------------------------------ */
/* Реакция на панель и мышь                                           */
/* ------------------------------------------------------------------ */

static gboolean on_size_changed (XfcePanelPlugin *plugin, gint size, OvpnPanel *panel){
    gint row_size = size / (gint) xfce_panel_plugin_get_nrows (plugin);
    (void) panel;
    if (xfce_panel_plugin_get_orientation (plugin) == GTK_ORIENTATION_HORIZONTAL)
        gtk_widget_set_size_request (GTK_WIDGET (plugin), -1, row_size);
    else
        gtk_widget_set_size_request (GTK_WIDGET (plugin), row_size, -1);
    return TRUE;
}

/* Во время передачи левая и средняя кнопки не работают, правая остаётся
 * панели — иначе до настроек было бы не добраться. */
static gboolean on_button_press (GtkWidget *widget, GdkEventButton *event, OvpnPanel *panel){
    (void) widget;
    return panel->busy && event->button != 3;
}

/* ------------------------------------------------------------------ */
/* Настройки                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    OvpnPanel *panel;
    GtkWidget *socket_entry;
    GtkWidget *timeout_spin;
    GtkWidget *font_button;
} OvpnDialog;

static void add_row (GtkWidget *grid, gint row, const gchar *caption, GtkWidget *control){
    GtkWidget *label = gtk_label_new(caption);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static void save_dialog_values(OvpnDialog *dialog){
    OvpnPanel *panel = dialog->panel;
    gchar     *font;
    g_free(panel->config.socket_path);
    panel->config.socket_path = g_strdup(gtk_entry_get_text(GTK_ENTRY(dialog->socket_entry)));
    panel->config.timeout_ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->timeout_spin));
    font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(dialog->font_button));
    if(font != NULL){
        g_free(panel->config.font);
        panel->config.font = font;
    }
    ovpn_config_save(&panel->config);
    reader_restart(panel);
    refresh_widget(panel);
}

static void
on_dialog_response(GtkWidget *window, gint response, OvpnDialog *dialog){
    if(response == GTK_RESPONSE_OK) save_dialog_values(dialog);
    xfce_panel_plugin_unblock_menu(dialog->panel->plugin);
    gtk_widget_destroy(window);
    g_free(dialog);
}

static void on_configure(XfcePanelPlugin *plugin, OvpnPanel *panel){
    OvpnDialog *dialog = g_new0(OvpnDialog, 1);
    GtkWidget  *window;
    GtkWidget  *grid;
    dialog->panel = panel;
    window = gtk_dialog_new_with_buttons("Настройки ovpn-widget", NULL,
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "Отмена", GTK_RESPONSE_CANCEL,
                                          "Сохранить", GTK_RESPONSE_OK,
                                          NULL);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    dialog->socket_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(dialog->socket_entry), panel->config.socket_path);
    gtk_entry_set_width_chars(GTK_ENTRY(dialog->socket_entry), 40);
    add_row(grid, 0, "Путь к сокету", dialog->socket_entry);

    dialog->timeout_spin = gtk_spin_button_new_with_range(50, 60000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->timeout_spin), panel->config.timeout_ms);
    add_row(grid, 1, "Таймаут чтения, мс", dialog->timeout_spin);

    dialog->font_button = gtk_font_button_new_with_font(panel->config.font);
    add_row(grid, 2, "Шрифт", dialog->font_button);

    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(window))), grid);
    g_signal_connect(window, "response", G_CALLBACK(on_dialog_response), dialog);

    xfce_panel_plugin_block_menu(plugin);
    xfce_panel_plugin_take_window(plugin, GTK_WINDOW(window));
    gtk_widget_show_all(window);
}

/* ------------------------------------------------------------------ */
/* Создание и разрушение плагина                                      */
/* ------------------------------------------------------------------ */

static void apply_frame_style(GtkWidget *frame){
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_widget_set_name(frame, "ovpn-frame");
    gtk_css_provider_load_from_data(provider, OVPN_FRAME_CSS, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(frame),
                                    GTK_STYLE_PROVIDER(provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void build_widgets(OvpnPanel *panel){
    panel->frame = gtk_event_box_new();
    panel->label = gtk_label_new(NULL);
    if(panel->frame == NULL || panel->label == NULL) ovpn_fatal("не удалось создать виджеты панели");

    gtk_event_box_set_visible_window(GTK_EVENT_BOX(panel->frame), FALSE);
    gtk_label_set_width_chars(GTK_LABEL(panel->label), 2);
    gtk_label_set_single_line_mode(GTK_LABEL(panel->label), TRUE);
    apply_frame_style(panel->frame);

    gtk_container_add(GTK_CONTAINER(panel->frame), panel->label);
    gtk_container_add(GTK_CONTAINER(panel->plugin), panel->frame);
    gtk_widget_show_all(panel->frame);

    g_signal_connect(panel->frame, "button-press-event",
                      G_CALLBACK(on_button_press), panel);
}

static void
on_free_data(XfcePanelPlugin *plugin, OvpnPanel *panel)
{
   (void) plugin;

    g_source_remove(panel->event_source);
    ovpn_reader_stop(panel->reader);
    ovpn_config_clear(&panel->config);
    g_free(panel->tooltip);
    g_free(panel);
}

static void
ovpn_construct(XfcePanelPlugin *plugin)
{
    OvpnPanel *panel = g_new0(OvpnPanel, 1);

    ovpn_log_open();

    panel->plugin = plugin;
    panel->count = OVPN_NO_DATA;
    ovpn_config_load(&panel->config);

    build_widgets(panel);
    refresh_widget(panel);

    xfce_panel_plugin_menu_show_configure(plugin);
    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(on_configure), panel);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed), panel);
    g_signal_connect(plugin, "free-data", G_CALLBACK(on_free_data), panel);

    reader_restart(panel);
}
