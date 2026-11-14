#ifndef KITTY_CLEANUP_H
#define KITTY_CLEANUP_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
typedef struct {
    char *path;
    char id[33];
    uint64_t bytes, dev, ino;
    int64_t modified;
    long nsec;
    unsigned category;
} KItem;
typedef struct { KItem *items; size_t count, capacity; unsigned skipped; } KList;
void kl_list_clear(KList *list);
/* Read-only scan, no symlink traversal, hidden descendants excluded, cap 5000. */
int kl_scan(const char *cache, const char *logs, const char *downloads, time_t now, KList *out);
/* Private, per-record manifests preserve recovery information before any move. */
int kl_store_prepare(const char *store);
int kl_quarantine(const char *store, const KItem *item);
int kl_quarantine_list(const char *store, KList *out);
int kl_restore(const char *store, const KItem *item);
int kl_release(const char *store, const KItem *item);
const char *kl_category(unsigned category);
#endif
