#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_CANDIDATES 5000
#define COL_SELECTED 0
#define COL_NAME 1
#define COL_PATH 2
#define COL_SIZE 3
#define COL_CATEGORY 4
#define COL_ID 5
#define COL_BYTES 6

typedef struct {
    GtkWidget *window, *status;
    GtkListStore *candidates, *quarantine;
    sqlite3 *db;
    char *data_dir, *quarantine_dir;
} App;

static App app;
static guint scan_entries;

static gboolean db_exec(const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(app.db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        g_warning("Kitty Litter database: %s", error ? error : sqlite3_errmsg(app.db));
        sqlite3_free(error);
        return FALSE;
    }
    return TRUE;
}

static void set_status(const char *message) {
    gtk_label_set_text(GTK_LABEL(app.status), message);
}

static gboolean safe_regular_file(GFileInfo *info) {
    return g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
           !g_file_info_get_is_symlink(info) &&
           !g_file_info_get_is_hidden(info);
}

static gboolean old_enough(GFileInfo *info, time_t cutoff) {
    GDateTime *modified = g_file_info_get_modification_date_time(info);
    if (!modified) return FALSE;
    time_t when = g_date_time_to_unix(modified);
    g_date_time_unref(modified);
    return when < cutoff;
}

static gboolean download_extension(const char *path) {
    char *lower = g_ascii_strdown(path, -1);
    gboolean match = g_str_has_suffix(lower, ".zip") ||
                     g_str_has_suffix(lower, ".dmg") ||
                     g_str_has_suffix(lower, ".pkg");
    g_free(lower);
    return match;
}

static void append_candidate(const char *path, guint64 size, const char *category) {
    GtkTreeIter iter;
    char *name = g_path_get_basename(path);
    char *size_text = g_format_size(size);
    gtk_list_store_append(app.candidates, &iter);
    gtk_list_store_set(app.candidates, &iter, COL_SELECTED, FALSE,
                       COL_NAME, name, COL_PATH, path, COL_SIZE, size_text,
                       COL_CATEGORY, category, COL_BYTES, size, -1);
    g_free(name); g_free(size_text);
}

static void scan_tree(const char *path, gboolean downloads, guint *count) {
    if (*count >= MAX_CANDIDATES) return;
    GFile *directory = g_file_new_for_path(path);
    GError *error = NULL;
    GFileInfo *root_info = g_file_query_info(directory,
        "standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (!root_info || g_file_info_get_file_type(root_info) != G_FILE_TYPE_DIRECTORY ||
        g_file_info_get_is_symlink(root_info)) {
        if (root_info) g_object_unref(root_info);
        g_clear_error(&error);
        g_object_unref(directory);
        return;
    }
    g_object_unref(root_info);
    GFileEnumerator *enumerator = g_file_enumerate_children(
        directory, "standard::name,standard::type,standard::is-hidden,standard::is-symlink,standard::size,time::modified",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (!enumerator) {
        g_clear_error(&error);
        g_object_unref(directory);
        return;
    }
    GFileInfo *info;
    time_t now = time(NULL);
    time_t cutoff = now - (downloads ? 60 : 30) * 24 * 60 * 60;
    while (*count < MAX_CANDIDATES && scan_entries < 100000 &&
           (info = g_file_enumerator_next_file(enumerator, NULL, NULL))) {
        scan_entries++;
        if (g_file_info_get_is_hidden(info) || g_file_info_get_is_symlink(info)) {
            g_object_unref(info);
            continue;
        }
        GFile *child = g_file_get_child(directory, g_file_info_get_name(info));
        char *child_path = g_file_get_path(child);
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            scan_tree(child_path, downloads, count);
        } else if (safe_regular_file(info) && old_enough(info, cutoff)) {
            guint64 size = g_file_info_get_size(info);
            const char *category = downloads ? "Old download" : "Cache or log";
            gboolean eligible = downloads
                ? size >= 50ULL * 1024 * 1024 && download_extension(child_path)
                : size >= 1024ULL * 1024;
            if (eligible) {
                append_candidate(child_path, size, category);
                (*count)++;
            }
        }
        g_free(child_path);
        g_object_unref(child);
        g_object_unref(info);
    }
    g_object_unref(enumerator);
    g_object_unref(directory);
}

static void scan_clicked(GtkButton *button, gpointer unused) {
    (void)button; (void)unused;
    gtk_list_store_clear(app.candidates);
    const char *home = g_get_home_dir();
    char *cache = g_build_filename(home, ".cache", NULL);
    char *state = g_build_filename(home, ".local", "state", NULL);
    char *downloads = g_build_filename(home, "Downloads", NULL);
    guint count = 0;
    scan_entries = 0;
    scan_tree(cache, FALSE, &count);
    scan_tree(state, FALSE, &count);
    scan_tree(downloads, TRUE, &count);
    char *message = g_strdup_printf("Found %u reviewable item(s). Nothing has been deleted.", count);
    set_status(message);
    g_free(message); g_free(cache); g_free(state); g_free(downloads);
}

static void toggle_cell(GtkCellRendererToggle *renderer, gchar *path_text, gpointer data) {
    (void)renderer;
    GtkTreeModel *model = GTK_TREE_MODEL(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_text);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gboolean selected;
        gtk_tree_model_get(model, &iter, COL_SELECTED, &selected, -1);
        gtk_list_store_set(GTK_LIST_STORE(model), &iter, COL_SELECTED, !selected, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *make_tree(GtkListStore *model, gboolean selectable) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
    if (selectable) {
        GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
        g_signal_connect(toggle, "toggled", G_CALLBACK(toggle_cell), model);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("Review", toggle, "active", COL_SELECTED, NULL));
    }
    const char *titles[] = {"Name", "Path", "Size", "Category"};
    int columns[] = {COL_NAME, COL_PATH, COL_SIZE, COL_CATEGORY};
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        if (columns[i] == COL_SIZE) g_object_set(renderer, "xalign", 1.0, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes(titles[i], renderer,
                columns[i] == COL_SIZE ? "text" : "text", columns[i], NULL));
    }
    return view;
}

static char *format_size(guint64 bytes) {
    return g_format_size(bytes);
}

static void refresh_quarantine(void) {
    gtk_list_store_clear(app.quarantine);
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(app.db,
        "SELECT id, original_path, payload_path, size, category FROM items WHERE state='quarantine' ORDER BY id DESC",
        -1, &statement, NULL) != SQLITE_OK) return;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *original = (const char *)sqlite3_column_text(statement, 1);
        const char *payload = (const char *)sqlite3_column_text(statement, 2);
        const char *category = (const char *)sqlite3_column_text(statement, 4);
        guint64 size = (guint64)sqlite3_column_int64(statement, 3);
        if (!g_file_test(payload, G_FILE_TEST_EXISTS)) continue;
        GtkTreeIter iter;
        char *name = g_path_get_basename(original);
        char *size_text = format_size(size);
        gtk_list_store_append(app.quarantine, &iter);
        gtk_list_store_set(app.quarantine, &iter, COL_SELECTED, FALSE, COL_NAME, name,
            COL_PATH, original, COL_SIZE, size_text, COL_CATEGORY, category,
            COL_ID, sqlite3_column_int64(statement, 0), COL_BYTES, size, -1);
        g_free(name); g_free(size_text);
    }
    sqlite3_finalize(statement);
}

static void quarantine_clicked(GtkButton *button, gpointer unused) {
    (void)button; (void)unused;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.candidates), &iter);
    guint moved = 0;
    while (valid) {
        gboolean selected;
        char *path = NULL, *category = NULL;
        guint64 size = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(app.candidates), &iter,
                           COL_SELECTED, &selected, COL_PATH, &path,
                           COL_CATEGORY, &category, COL_BYTES, &size, -1);
        GtkTreeIter next = iter;
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app.candidates), &next);
        if (selected) {
            GFile *source = g_file_new_for_path(path);
            GError *query_error = NULL;
            GFileInfo *current = g_file_query_info(source,
                "standard::type,standard::is-symlink,standard::size,time::modified",
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &query_error);
            if (!current || !safe_regular_file(current) ||
                (guint64)g_file_info_get_size(current) != size) {
                g_warning("Quarantine skipped changed or unsupported path %s", path);
                if (query_error) g_clear_error(&query_error);
                if (current) g_object_unref(current);
                g_object_unref(source);
                g_free(path); g_free(category);
                iter = next;
                continue;
            }
            g_object_unref(current);
            char *basename = g_path_get_basename(path);
            char *destination = g_strdup_printf("%s/%u-%s", app.quarantine_dir, g_random_int(), basename);
            GFile *target = g_file_new_for_path(destination);
            GError *error = NULL;
            if (g_file_move(source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &error)) {
                sqlite3_stmt *stmt = NULL;
                gboolean indexed = FALSE;
                if (sqlite3_prepare_v2(app.db,
                    "INSERT INTO items(original_path,payload_path,size,category,state) VALUES(?,?,?,?, 'quarantine')",
                    -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, destination, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)size);
                    sqlite3_bind_text(stmt, 4, category, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(stmt) == SQLITE_DONE) { moved++; indexed = TRUE; }
                    else g_warning("Quarantine index write failed: %s", sqlite3_errmsg(app.db));
                }
                sqlite3_finalize(stmt);
                if (!indexed) {
                    GError *rollback_error = NULL;
                    if (!g_file_move(target, source, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                     NULL, NULL, NULL, &rollback_error)) {
                        g_warning("Could not roll back an unindexed quarantine move: %s",
                                  rollback_error->message);
                        g_clear_error(&rollback_error);
                    }
                }
            } else {
                g_warning("Could not quarantine %s: %s", path, error->message);
                g_clear_error(&error);
            }
            g_object_unref(source); g_object_unref(target);
            g_free(basename); g_free(destination);
        }
        g_free(path); g_free(category);
        iter = next;
    }
    gtk_list_store_clear(app.candidates);
    refresh_quarantine();
    char *message = g_strdup_printf("Moved %u item(s) to quarantine. Nothing has been permanently deleted.", moved);
    set_status(message); g_free(message);
}

static void operate_on_selected_quarantine(gboolean release) {
    if (release) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_CANCEL,
            "Permanently delete the selected quarantined items?");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Delete Permanently", GTK_RESPONSE_ACCEPT);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "This cannot be undone. Items are deleted only from Kitty Litter's quarantine.");
        gboolean accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(dialog);
        if (!accepted) return;
    }
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.quarantine), &iter);
    guint changed = 0;
    while (valid) {
        gboolean selected;
        char *original = NULL;
        gint64 id = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(app.quarantine), &iter,
            COL_SELECTED, &selected, COL_PATH, &original,
            COL_ID, &id, -1);
        GtkTreeIter next = iter;
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app.quarantine), &next);
        if (selected) {
            GtkTreeModel *model = GTK_TREE_MODEL(app.quarantine);
            char *real_payload = NULL;
            gtk_tree_model_get(model, &iter, COL_ID, &id, -1);
            /* Payload paths are retrieved directly from the durable index. */
            sqlite3_stmt *lookup = NULL;
            if (sqlite3_prepare_v2(app.db, "SELECT payload_path FROM items WHERE id=? AND state='quarantine'", -1, &lookup, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(lookup, 1, id);
                if (sqlite3_step(lookup) == SQLITE_ROW) {
                    const char *value = (const char *)sqlite3_column_text(lookup, 0);
                    if (value) real_payload = g_strdup(value);
                }
            }
            sqlite3_finalize(lookup);
            if (real_payload) {
                GFile *source = g_file_new_for_path(real_payload);
                GFile *destination = g_file_new_for_path(original);
                GError *error = NULL;
                gboolean ok = FALSE;
                if (release) {
                    ok = g_file_delete(source, NULL, &error);
                } else if (g_file_test(original, G_FILE_TEST_EXISTS)) {
                    g_warning("Restore skipped because the original path already exists: %s", original);
                } else {
                    char *parent = g_path_get_dirname(original);
                    if (g_mkdir_with_parents(parent, 0700) == 0)
                        ok = g_file_move(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &error);
                    else
                        g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create restore directory: %s", g_strerror(errno));
                    g_free(parent);
                }
                if (ok) {
                    sqlite3_stmt *remove = NULL;
                    if (sqlite3_prepare_v2(app.db, "DELETE FROM items WHERE id=?", -1, &remove, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(remove, 1, id);
                        if (sqlite3_step(remove) == SQLITE_DONE) changed++;
                    }
                    sqlite3_finalize(remove);
                } else if (error) {
                    g_warning("Quarantine action failed for %s: %s", original, error->message);
                    g_clear_error(&error);
                }
                g_object_unref(source); g_object_unref(destination); g_free(real_payload);
            }
        }
        g_free(original);
        iter = next;
    }
    refresh_quarantine();
    set_status(release ? (changed ? "Selected quarantined files were permanently deleted." : "No quarantined files were deleted.")
                       : (changed ? "Selected files were restored to their original paths." : "No files were restored."));
}

static void restore_clicked(GtkButton *button, gpointer unused) {
    (void)button; (void)unused;
    operate_on_selected_quarantine(FALSE);
}

static void release_clicked(GtkButton *button, gpointer unused) {
    (void)button; (void)unused;
    operate_on_selected_quarantine(TRUE);
}

static void activate(GtkApplication *application, gpointer unused) {
    (void)unused;
    app.window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app.window), "Kitty Litter");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 560);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(app.window), root);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new("Kitty Litter · Review before cleanup");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    GtkWidget *scan = gtk_button_new_with_label("Scan Now");
    g_signal_connect(scan, "clicked", G_CALLBACK(scan_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(header), scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    app.candidates = gtk_list_store_new(7, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT64);
    app.quarantine = gtk_list_store_new(7, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT64);
    GtkWidget *tabs = gtk_notebook_new();
    GtkWidget *candidate_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(candidate_scroll), make_tree(app.candidates, TRUE));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), candidate_scroll, gtk_label_new("Candidates"));
    GtkWidget *quarantine_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(quarantine_scroll), make_tree(app.quarantine, TRUE));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), quarantine_scroll, gtk_label_new("Quarantine"));
    gtk_box_pack_start(GTK_BOX(root), tabs, TRUE, TRUE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *quarantine = gtk_button_new_with_label("Move Selected to Quarantine");
    g_signal_connect(quarantine, "clicked", G_CALLBACK(quarantine_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(actions), quarantine, FALSE, FALSE, 0);
    GtkWidget *restore = gtk_button_new_with_label("Restore Selected");
    g_signal_connect(restore, "clicked", G_CALLBACK(restore_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(actions), restore, FALSE, FALSE, 0);
    GtkWidget *release = gtk_button_new_with_label("Release Permanently…");
    g_signal_connect(release, "clicked", G_CALLBACK(release_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(actions), release, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    app.status = gtk_label_new("Nothing is deleted until you review and quarantine it.");
    gtk_widget_set_halign(app.status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), app.status, FALSE, FALSE, 0);
    refresh_quarantine();
    gtk_widget_show_all(app.window);
}

int main(int argc, char **argv) {
    const char *base = g_get_user_data_dir();
    app.data_dir = g_build_filename(base, "kitty-litter", NULL);
    app.quarantine_dir = g_build_filename(app.data_dir, "quarantine", NULL);
    if (g_mkdir_with_parents(app.quarantine_dir, 0700) != 0) {
        g_printerr("Kitty Litter cannot create its private quarantine directory: %s\n", g_strerror(errno));
        return 1;
    }
    g_chmod(app.data_dir, 0700);
    g_chmod(app.quarantine_dir, 0700);
    char *db_path = g_build_filename(app.data_dir, "index.sqlite3", NULL);
    if (sqlite3_open_v2(db_path, &app.db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK ||
        !db_exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, original_path TEXT NOT NULL, payload_path TEXT NOT NULL, size INTEGER NOT NULL, category TEXT NOT NULL, state TEXT NOT NULL)")) {
        g_printerr("Kitty Litter cannot open its quarantine index.\n");
        return 1;
    }
    g_chmod(db_path, 0600);
    g_free(db_path);
    GtkApplication *application = gtk_application_new("org.neobell.kitty-litter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int result = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    sqlite3_close(app.db);
    g_free(app.data_dir); g_free(app.quarantine_dir);
    return result;
}
