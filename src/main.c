#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <glib/gstdio.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "cleanup.h"

enum { SELECTED, NAME, CATEGORY, SIZE, PATH, INDEX, BYTES, COLUMNS };
typedef enum { SCAN, QUARANTINE, RESTORE, RELEASE } Operation;
typedef struct {
    Operation operation;
    KList chosen, candidates, quarantine;
    unsigned succeeded, failed;
    char *first_error;
} Job;
static GtkWidget *window, *notebook, *status_label, *selected_label, *disk_label;
static GtkWidget *scan_button, *move_button, *restore_button, *release_button;
static GtkListStore *candidate_model, *quarantine_model;
static KList candidates, quarantine;
static AppIndicator *indicator;
static char *cache_path, *log_path, *download_path, *store_path, *icon_dir;
static gboolean busy;

static void error_dialog(const char *text)
{
    GtkWidget *dialog=gtk_message_dialog_new(GTK_WINDOW(window),GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",text);
    gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}
static guint selection_count(GtkListStore *model, guint64 *bytes)
{
    GtkTreeIter iter; guint count=0; *bytes=0;
    gboolean valid=gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model),&iter);
    while(valid) {
        gboolean selected; guint64 size;
        gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,SELECTED,&selected,BYTES,&size,-1);
        if(selected) { ++count; *bytes=G_MAXUINT64-*bytes<size ? G_MAXUINT64 : *bytes+size; }
        valid=gtk_tree_model_iter_next(GTK_TREE_MODEL(model),&iter);
    }
    return count;
}
static void update_controls(void)
{
    guint64 bytes,ignored;
    guint nc=selection_count(candidate_model,&bytes);
    guint nq=selection_count(quarantine_model,&ignored);
    char *size=g_format_size(bytes),*text=g_strdup_printf("Selected candidates: %s",size);
    gtk_label_set_text(GTK_LABEL(selected_label),text); g_free(size); g_free(text);
    gtk_widget_set_sensitive(scan_button,!busy);
    gtk_widget_set_sensitive(move_button,!busy&&nc>0);
    gtk_widget_set_sensitive(restore_button,!busy&&nq>0);
    gtk_widget_set_sensitive(release_button,!busy&&nq>0);
    gtk_widget_set_sensitive(notebook,!busy);
}
static void checkbox(GtkCellRendererToggle *cell, char *path, gpointer data)
{
    (void)cell; if(busy) return;
    GtkListStore *model=data; GtkTreeIter iter;
    if(gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(model),&iter,path)) {
        gboolean selected; gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,SELECTED,&selected,-1);
        gtk_list_store_set(model,&iter,SELECTED,!selected,-1); update_controls();
    }
}
static GtkWidget *table(GtkListStore **model)
{
    *model=gtk_list_store_new(COLUMNS,G_TYPE_BOOLEAN,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,
        G_TYPE_STRING,G_TYPE_UINT,G_TYPE_UINT64);
    GtkWidget *view=gtk_tree_view_new_with_model(GTK_TREE_MODEL(*model));
    GtkCellRenderer *toggle=gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle,"toggled",G_CALLBACK(checkbox),*model);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes("",toggle,"active",SELECTED,NULL));
    const char *labels[]={"Name","Category","Size","Path"};
    const int fields[]={NAME,CATEGORY,SIZE,PATH};
    for(int i=0;i<4;++i) {
        GtkCellRenderer *text=gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col=gtk_tree_view_column_new_with_attributes(labels[i],text,"text",fields[i],NULL);
        gtk_tree_view_column_set_resizable(col,TRUE);
        if(i==3) gtk_tree_view_column_set_expand(col,TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),col);
    }
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_container_add(GTK_CONTAINER(scroll),view); return scroll;
}
static void fill(GtkListStore *model,const KList *list)
{
    gtk_list_store_clear(model);
    for(size_t i=0;i<list->count;++i) {
        const KItem *item=&list->items[i]; GtkTreeIter iter;
        char *path=g_filename_display_name(item->path);
        char *name=g_filename_display_basename(item->path),*size=g_format_size(item->bytes);
        gtk_list_store_append(model,&iter);
        gtk_list_store_set(model,&iter,SELECTED,FALSE,NAME,name,CATEGORY,kl_category(item->category),
            SIZE,size,PATH,path,INDEX,(guint)i,BYTES,(guint64)item->bytes,-1);
        g_free(name); g_free(path); g_free(size);
    }
}
static void job_free(Job *job)
{
    kl_list_clear(&job->chosen); kl_list_clear(&job->candidates); kl_list_clear(&job->quarantine);
    g_free(job->first_error); g_free(job);
}
static void job_error(Job *job,const char *path,int saved)
{
    ++job->failed;
    if(!job->first_error) {
        char *name=g_filename_display_name(path);
        const char *reason=saved==EXDEV ? "Different filesystem: choose a quarantine location on the same filesystem." :
            saved==ESTALE ? "The file changed since the scan, or the quarantine payload no longer matches its record." :
            saved==EEXIST ? "A destination already exists; it was not replaced." : g_strerror(saved);
        job->first_error=g_strdup_printf("%s: %s",name,reason); g_free(name);
    }
}
static void worker(GTask *task,gpointer source,gpointer data,GCancellable *cancel)
{
    (void)source; (void)cancel; Job *job=data;
    for(size_t i=0;i<job->chosen.count;++i) {
        KItem *item=&job->chosen.items[i]; int result;
        if(job->operation==QUARANTINE) result=kl_quarantine(store_path,item);
        else if(job->operation==RESTORE) result=kl_restore(store_path,item);
        else result=kl_release(store_path,item);
        if(result) job_error(job,item->path,errno); else ++job->succeeded;
    }
    if(kl_scan(cache_path,log_path,download_path,time(NULL),&job->candidates)) job_error(job,"Scan",errno);
    /* A configured scan root can contain the quarantine. Never offer its payloads. */
    size_t prefix=strlen(store_path);
    for(size_t i=0;i<job->candidates.count;) {
        char *p=job->candidates.items[i].path;
        if(!strncmp(p,store_path,prefix)&&(p[prefix]=='/'||p[prefix]==0)) {
            g_free(p); job->candidates.items[i]=job->candidates.items[--job->candidates.count];
        } else ++i;
    }
    if(kl_quarantine_list(store_path,&job->quarantine)) job_error(job,"Quarantine index",errno);
    g_task_return_boolean(task,TRUE);
}
static void finished(GObject *source,GAsyncResult *result,gpointer data)
{
    (void)source; (void)data;
    Job *job=g_task_get_task_data(G_TASK(result));
    (void)g_task_propagate_boolean(G_TASK(result),NULL);
    kl_list_clear(&candidates); candidates=job->candidates; memset(&job->candidates,0,sizeof job->candidates);
    kl_list_clear(&quarantine); quarantine=job->quarantine; memset(&job->quarantine,0,sizeof job->quarantine);
    fill(candidate_model,&candidates); fill(quarantine_model,&quarantine);
    busy=FALSE; update_controls();
    char *message;
    if(job->operation==SCAN) message=g_strdup_printf("Found %zu reviewable items · %u paths skipped · %u incomplete/invalid quarantine records",
        candidates.count,candidates.skipped,quarantine.skipped);
    else message=g_strdup_printf("%u item(s) %s · %u failed/skipped",job->succeeded,
        job->operation==QUARANTINE ? "quarantined" : job->operation==RESTORE ? "restored" : "permanently released",job->failed);
    gtk_label_set_text(GTK_LABEL(status_label),message); g_free(message);
    struct statvfs disk;
    if(!statvfs(g_get_home_dir(),&disk)) {
        guint64 available=(guint64)disk.f_bavail*(guint64)disk.f_frsize;
        char *size=g_format_size(available);
        message=g_strdup_printf("%s%s available",available<UINT64_C(20000000000)?"Low disk space · ":"",size);
        gtk_label_set_text(GTK_LABEL(disk_label),message); g_free(size); g_free(message);
    }
    if(job->first_error) error_dialog(job->first_error);
}
static void start(Operation operation)
{
    if(busy) return;
    Job *job=g_new0(Job,1); job->operation=operation;
    if(operation!=SCAN) {
        GtkListStore *model=operation==QUARANTINE ? candidate_model : quarantine_model;
        const KList *list=operation==QUARANTINE ? &candidates : &quarantine;
        job->chosen.items=calloc(list->count,sizeof(KItem));
        if(list->count&&!job->chosen.items) { job_free(job); error_dialog("Not enough memory."); return; }
        GtkTreeIter iter;
        gboolean valid=gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model),&iter);
        while(valid) {
            gboolean selected; guint index;
            gtk_tree_model_get(GTK_TREE_MODEL(model),&iter,SELECTED,&selected,INDEX,&index,-1);
            if(selected && index<list->count) {
                KItem item=list->items[index]; item.path=g_strdup(item.path);
                job->chosen.items[job->chosen.count++]=item;
            }
            valid=gtk_tree_model_iter_next(GTK_TREE_MODEL(model),&iter);
        }
        if(!job->chosen.count) { job_free(job); return; }
        if(operation==RELEASE) {
            GtkWidget *dialog=gtk_message_dialog_new(GTK_WINDOW(window),GTK_DIALOG_MODAL,GTK_MESSAGE_WARNING,
                GTK_BUTTONS_NONE,"Permanently release %zu item(s)?",job->chosen.count);
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"This deletes the selected quarantined files and cannot be undone.");
            gtk_dialog_add_buttons(GTK_DIALOG(dialog),"Cancel",GTK_RESPONSE_CANCEL,"Release Permanently",GTK_RESPONSE_ACCEPT,NULL);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
            int response=gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
            if(response!=GTK_RESPONSE_ACCEPT) { job_free(job); return; }
        }
    }
    busy=TRUE; update_controls(); gtk_label_set_text(GTK_LABEL(status_label),operation==SCAN?"Scanning…":"Working…");
    GTask *task=g_task_new(NULL,NULL,finished,NULL);
    g_task_set_task_data(task,job,(GDestroyNotify)job_free);
    g_task_run_in_thread(task,worker); g_object_unref(task);
}
static void scan_clicked(GtkWidget *widget,gpointer data) { (void)widget; (void)data; start(SCAN); }
static void move_clicked(GtkWidget *widget,gpointer data) { (void)widget; (void)data; start(QUARANTINE); }
static void restore_clicked(GtkWidget *widget,gpointer data) { (void)widget; (void)data; start(RESTORE); }
static void release_clicked(GtkWidget *widget,gpointer data) { (void)widget; (void)data; start(RELEASE); }
static void open_clicked(GtkWidget *widget,gpointer data) { (void)widget; (void)data; gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); }
static gboolean hide_window(GtkWidget *widget,GdkEvent *event,gpointer data)
{ (void)event; (void)data; gtk_widget_hide(widget); return TRUE; }
static void quit_clicked(GtkWidget *widget,gpointer data)
{
    (void)widget; (void)data;
    if(busy) { error_dialog("Wait for the current operation to finish before quitting."); return; }
    gtk_main_quit();
}
static gboolean scheduled_scan(gpointer data) { (void)data; start(SCAN); return G_SOURCE_CONTINUE; }
static GtkWidget *button(GtkWidget *box,const char *label,GCallback callback)
{
    GtkWidget *b=gtk_button_new_with_label(label); g_signal_connect(b,"clicked",callback,NULL);
    gtk_box_pack_start(GTK_BOX(box),b,FALSE,FALSE,0); return b;
}
static char *configured(const char *variable,const char *base,const char *leaf)
{
    const char *override=g_getenv(variable);
    char *raw=override&&*override ? g_strdup(override) : g_build_filename(base,leaf,NULL);
    char *absolute=g_canonicalize_filename(raw,NULL); g_free(raw); return absolute;
}
int main(int argc,char **argv)
{
    gboolean dark_outline=FALSE;
    for(int i=1;i<argc;++i) {
        if(!g_strcmp0(argv[i],"--dark-outline")) dark_outline=TRUE;
        else if(!g_strcmp0(argv[i],"--help")) { g_print("kitty-litter [--dark-outline]\nReview old large files before quarantine or deletion.\n"); return 0; }
        else { g_printerr("Unknown option: %s\n",argv[i]); return 2; }
    }
    if(!gtk_init_check(NULL,NULL)) { g_printerr("Kitty Litter requires a graphical desktop.\n"); return 1; }
    cache_path=configured("KITTY_LITTER_CACHE_DIR",g_get_user_cache_dir(),"");
    const char *state=g_getenv("XDG_STATE_HOME");
    char *state_default=g_build_filename(g_get_home_dir(),".local","state",NULL);
    log_path=configured("KITTY_LITTER_LOG_DIR",state&&*state ? state : state_default,"log"); g_free(state_default);
    const char *downloads=g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    download_path=configured("KITTY_LITTER_DOWNLOAD_DIR",downloads ? downloads : g_get_home_dir(),downloads ? "" : "Downloads");
    store_path=configured("KITTY_LITTER_QUARANTINE",g_get_user_data_dir(),"kitty-litter/quarantine");
    window=gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(window),"Kitty Litter");
    gtk_window_set_default_size(GTK_WINDOW(window),900,540);
    g_signal_connect(window,"delete-event",G_CALLBACK(hide_window),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10); gtk_container_set_border_width(GTK_CONTAINER(box),16);
    gtk_container_add(GTK_CONTAINER(window),box);
    GtkWidget *header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10); gtk_box_pack_start(GTK_BOX(box),header,FALSE,FALSE,0);
    GtkWidget *title=gtk_label_new("Kitty Litter"); gtk_box_pack_start(GTK_BOX(header),title,FALSE,FALSE,0);
    disk_label=gtk_label_new("Checking free space…"); gtk_box_pack_start(GTK_BOX(header),disk_label,TRUE,TRUE,0);
    scan_button=button(header,"Scan Now",G_CALLBACK(scan_clicked));
    notebook=gtk_notebook_new(); gtk_box_pack_start(GTK_BOX(box),notebook,TRUE,TRUE,0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),table(&candidate_model),gtk_label_new("Candidates"));
    GtkWidget *quarantine_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_box_pack_start(GTK_BOX(quarantine_box),table(&quarantine_model),TRUE,TRUE,0);
    GtkWidget *actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_box_pack_start(GTK_BOX(quarantine_box),actions,FALSE,FALSE,0);
    restore_button=button(actions,"Restore",G_CALLBACK(restore_clicked));
    release_button=button(actions,"Release Permanently…",G_CALLBACK(release_clicked));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),quarantine_box,gtk_label_new("Quarantine"));
    status_label=gtk_label_new("Ready to scan"); gtk_label_set_xalign(GTK_LABEL(status_label),0);
    gtk_box_pack_start(GTK_BOX(box),status_label,FALSE,FALSE,0);
    GtkWidget *bottom=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); gtk_box_pack_start(GTK_BOX(box),bottom,FALSE,FALSE,0);
    selected_label=gtk_label_new("Selected candidates: 0 bytes"); gtk_box_pack_start(GTK_BOX(bottom),selected_label,TRUE,TRUE,0);
    move_button=button(bottom,"Pound into Quarantine",G_CALLBACK(move_clicked));
    button(bottom,"Quit",G_CALLBACK(quit_clicked));
    GError *error=NULL; icon_dir=g_dir_make_tmp("kitty-litter-icon-XXXXXX",&error);
    if(!icon_dir) { error_dialog(error->message); g_clear_error(&error); return 1; }
    char *icon=g_build_filename(icon_dir,"kitty-litter.svg",NULL);
    const char *white="<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 18\"><g transform=\"translate(0 18) scale(1 -1)\" fill=\"white\" stroke=\"white\" stroke-linejoin=\"round\"><path fill=\"none\" stroke-width=\"1.7\" d=\"M2.2 11.5H17.8L16.1 3.4Q15.9 1.8 13.5 1.8H6.5Q4.1 1.8 3.9 3.4Z\"/><circle cx=\"6.7\" cy=\"5.7\" r=\".7\"/><circle cx=\"9.8\" cy=\"4.9\" r=\".7\"/><circle cx=\"13.8\" cy=\"5.9\" r=\".7\"/><ellipse fill=\"none\" stroke-width=\"1.5\" cx=\"6.7\" cy=\"12.45\" rx=\"2.5\" ry=\"2.05\"/><path stroke-width=\"2\" stroke-linecap=\"round\" d=\"M8.7 13.6L14.6 17\"/></g></svg>";
    char *svg=g_strdup(white);
    if(dark_outline) { char **parts=g_strsplit(svg,"white",-1); g_free(svg); svg=g_strjoinv("black",parts); g_strfreev(parts); }
    if(!g_file_set_contents(icon,svg,-1,&error)) { error_dialog(error->message); g_clear_error(&error); return 1; }
    g_free(svg);
    indicator=app_indicator_new("kitty-litter","kitty-litter",APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_title(indicator,"Kitty Litter"); app_indicator_set_icon_theme_path(indicator,icon_dir);
    GtkWidget *menu=gtk_menu_new();
    const char *labels[]={"Open Review Window","Scan Now","Quit Kitty Litter"};
    GCallback callbacks[]={G_CALLBACK(open_clicked),G_CALLBACK(scan_clicked),G_CALLBACK(quit_clicked)};
    for(int i=0;i<3;++i) { GtkWidget *item=gtk_menu_item_new_with_label(labels[i]);
        g_signal_connect(item,"activate",callbacks[i],NULL); gtk_menu_shell_append(GTK_MENU_SHELL(menu),item); }
    gtk_widget_show_all(menu); app_indicator_set_menu(indicator,GTK_MENU(menu));
    app_indicator_set_status(indicator,APP_INDICATOR_STATUS_ACTIVE);
    gtk_widget_show_all(window); update_controls(); start(SCAN);
    guint periodic=g_timeout_add_seconds(6*60*60,scheduled_scan,NULL);
    gtk_main(); g_source_remove(periodic);
    g_object_unref(indicator); gtk_widget_destroy(menu); gtk_widget_destroy(window);
    g_object_unref(candidate_model); g_object_unref(quarantine_model);
    kl_list_clear(&candidates); kl_list_clear(&quarantine);
    g_remove(icon); g_rmdir(icon_dir); g_free(icon); g_free(icon_dir);
    g_free(cache_path); g_free(log_path); g_free(download_path); g_free(store_path);
    return 0;
}
