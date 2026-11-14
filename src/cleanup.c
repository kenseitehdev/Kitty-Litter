#define _GNU_SOURCE
#include "cleanup.h"
#include <sys/stat.h>
#include <sys/random.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#define MAX_ITEMS 5000
#define MAX_PATH_BYTES 65536

static int safe_name(const char *name)
{ return *name && strcmp(name,".") && strcmp(name,"..") && !strchr(name,'/'); }
/* Resolve each directory by descriptor, never traversing a symbolic link. */
static int directory(const char *path, int create)
{
    if (!path || path[0] != '/') { errno=EINVAL; return -1; }
    char *copy=strdup(path); if (!copy) return -1;
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    char *save=NULL, *part=strtok_r(copy,"/",&save);
    while (fd>=0 && part) {
        if (!safe_name(part)) { close(fd); fd=-1; errno=EINVAL; break; }
        if (create && mkdirat(fd,part,0700) && errno!=EEXIST) { close(fd); fd=-1; break; }
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int error=errno; close(fd); fd=next; errno=error;
        part=strtok_r(NULL,"/",&save);
    }
    free(copy); return fd;
}
static int parent(const char *path, char **leaf, int create)
{
    char *copy=strdup(path); if (!copy) return -1;
    char *slash=strrchr(copy,'/');
    if (!slash || !safe_name(slash+1)) { free(copy); errno=EINVAL; return -1; }
    *leaf=strdup(slash+1); if (!*leaf) { free(copy); return -1; }
    if (slash==copy) slash[1]=0; else *slash=0;
    int fd=directory(copy,create); free(copy);
    if (fd<0) { free(*leaf); *leaf=NULL; }
    return fd;
}
static int private_store(const char *path)
{
    int fd=directory(path,0); if (fd<0) return -1;
    struct stat st;
    if (fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        close(fd); errno=EACCES; return -1;
    }
    return fd;
}
int kl_store_prepare(const char *store)
{
    int fd=directory(store,1); if (fd<0) return -1;
    close(fd); fd=private_store(store); if (fd<0) return -1;
    close(fd); return 0;
}
void kl_list_clear(KList *list)
{
    for (size_t i=0;i<list->count;++i) free(list->items[i].path);
    free(list->items); memset(list,0,sizeof *list);
}
static int append(KList *list, KItem item)
{
    if (list->count==list->capacity) {
        size_t n=list->capacity ? list->capacity*2 : 32;
        KItem *items=realloc(list->items,n*sizeof *items); if (!items) return -1;
        list->items=items; list->capacity=n;
    }
    list->items[list->count++]=item; return 0;
}
const char *kl_category(unsigned c)
{ return c==0 ? "Cache" : c==1 ? "Log" : "Old download"; }
static int download_extension(const char *name)
{
    const char *ext=strrchr(name,'.'); if (!ext) return 0;
    const char *allowed[]={".zip",".deb",".rpm",".AppImage",".tar",".gz",".xz",".bz2",".zst",".7z",".dmg",".pkg"};
    for(size_t i=0;i<sizeof allowed/sizeof *allowed;++i) if(!strcasecmp(ext,allowed[i])) return 1;
    return 0;
}
static int walk(int fd, const char *path, unsigned category, time_t cutoff, KList *list, unsigned depth)
{
    if (depth>128) { close(fd); list->skipped++; return 0; }
    DIR *dir=fdopendir(fd); if (!dir) { close(fd); return -1; }
    int result=0;
    struct dirent *entry;
    while(list->count<MAX_ITEMS && (entry=readdir(dir))) {
        if(entry->d_name[0]=='.') continue;
        struct stat st;
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)) { list->skipped++; continue; }
        char *child=NULL;
        if(asprintf(&child,"%s/%s",path,entry->d_name)<0) { result=-1; break; }
        if(S_ISDIR(st.st_mode)) {
            int next=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(next<0) list->skipped++;
            else if(walk(next,child,category,cutoff,list,depth+1)) { free(child); result=-1; break; }
        } else if(S_ISREG(st.st_mode) && st.st_size>=0 && st.st_mtime<cutoff &&
            (uint64_t)st.st_size>=(category==2 ? UINT64_C(50)*1048576 : 1048576) &&
            (category!=2 || download_extension(entry->d_name))) {
            KItem item={.path=child,.bytes=(uint64_t)st.st_size,.dev=st.st_dev,.ino=st.st_ino,
                .modified=st.st_mtim.tv_sec,.nsec=st.st_mtim.tv_nsec,.category=category};
            if(append(list,item)) { free(child); result=-1; break; }
            child=NULL;
        }
        free(child);
    }
    closedir(dir); return result;
}
static int descending(const void *a,const void *b)
{
    const KItem *x=a,*y=b;
    return x->bytes==y->bytes ? strcmp(x->path,y->path) : x->bytes<y->bytes ? 1 : -1;
}
int kl_scan(const char *cache,const char *logs,const char *downloads,time_t now,KList *out)
{
    const char *roots[]={cache,logs,downloads};
    memset(out,0,sizeof *out);
    for(unsigned i=0;i<3;++i) {
        int fd=directory(roots[i],0);
        if(fd<0) { out->skipped++; continue; }
        if(walk(fd,roots[i],i,now-(i==2?60:30)*24*60*60,out,0)) { kl_list_clear(out); return -1; }
    }
    /* Overlapping configured roots must not produce duplicate operations. */
    for(size_t i=0;i<out->count;++i) for(size_t j=i+1;j<out->count;) {
        if(!strcmp(out->items[i].path,out->items[j].path)) {
            free(out->items[j].path); out->items[j]=out->items[--out->count];
        } else ++j;
    }
    if(out->count>1) qsort(out->items,out->count,sizeof *out->items,descending);
    return 0;
}
static int same(const struct stat *st,const KItem *item)
{
    return S_ISREG(st->st_mode) && st->st_uid==geteuid() && (uint64_t)st->st_dev==item->dev &&
        (uint64_t)st->st_ino==item->ino && (uint64_t)st->st_size==item->bytes &&
        st->st_mtim.tv_sec==item->modified && st->st_mtim.tv_nsec==item->nsec;
}
static int id_valid(const char *id)
{
    if(strlen(id)!=32) return 0;
    for(int i=0;i<32;++i) if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f'))) return 0;
    return 1;
}
static int put64(FILE *f,uint64_t n)
{
    unsigned char b[8]; for(int i=0;i<8;++i) { b[i]=(unsigned char)n; n>>=8; }
    return fwrite(b,1,8,f)==8 ? 0 : -1;
}
static int get64(FILE *f,uint64_t *n)
{
    unsigned char b[8]; if(fread(b,1,8,f)!=8) return -1;
    *n=0; for(int i=7;i>=0;--i) *n=(*n<<8)|b[i]; return 0;
}
static int manifest(int dir,const KItem *item)
{
    int fd=openat(dir,"record",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return -1;
    FILE *f=fdopen(fd,"wb"); if(!f) { close(fd); return -1; }
    size_t length=strlen(item->path);
    int result=0;
    if(length>MAX_PATH_BYTES || fwrite("KLT1",1,4,f)!=4 || put64(f,length) || put64(f,item->bytes) ||
       put64(f,(uint64_t)item->modified) || put64(f,(uint64_t)item->nsec) || put64(f,item->dev) || put64(f,item->ino) ||
       put64(f,item->category) || fwrite(item->path,1,length,f)!=length || fflush(f) || fsync(fd)) result=-1;
    if(fclose(f)) result=-1;
    return result;
}
static int read_manifest(int dir,KItem *item)
{
    memset(item,0,sizeof *item);
    int fd=openat(dir,"record",O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if(fd<0) return -1;
    FILE *f=fdopen(fd,"rb"); if(!f) { close(fd); return -1; }
    char magic[4]; uint64_t length,mtime,nsec,category;
    int result=-1;
    if(fread(magic,1,4,f)!=4 || memcmp(magic,"KLT1",4) || get64(f,&length) || !length || length>MAX_PATH_BYTES ||
        get64(f,&item->bytes) || get64(f,&mtime) || get64(f,&nsec) || get64(f,&item->dev) || get64(f,&item->ino) ||
        get64(f,&category) || category>2 || nsec>999999999) goto done;
    item->path=malloc((size_t)length+1); if(!item->path) goto done;
    if(fread(item->path,1,(size_t)length,f)!=length || fgetc(f)!=EOF || ferror(f) || memchr(item->path,0,(size_t)length)) goto done;
    item->path[length]=0;
    if(item->path[0]!='/') goto done;
    item->modified=mtime<=INT64_MAX ? (int64_t)mtime : -1-(int64_t)(UINT64_MAX-mtime); item->nsec=(long)nsec; item->category=(unsigned)category; result=0;
done:
    fclose(f); if(result) { free(item->path); item->path=NULL; errno=EINVAL; } return result;
}
static int entry_directory(int store,const char *id)
{
    if(!id_valid(id)) { errno=EINVAL; return -1; }
    int fd=openat(store,id,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0) {
        struct stat st;
        if(fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) { close(fd); errno=EACCES; return -1; }
    }
    return fd;
}
int kl_quarantine(const char *store,const KItem *item)
{
    if(kl_store_prepare(store)) return -1;
    int root=private_store(store); if(root<0) return -1;
    char *leaf=NULL; int source=parent(item->path,&leaf,0);
    int dir=-1,result=-1; char id[33]=""; struct stat st;
    if(source<0) goto done;
    if(fstatat(source,leaf,&st,AT_SYMLINK_NOFOLLOW) || !same(&st,item)) { errno=ESTALE; goto done; }
    unsigned char random[16];
    if(getrandom(random,sizeof random,0)!=(ssize_t)sizeof random) { if(!errno) errno=EIO; goto done; }
    for(int i=0;i<16;++i) snprintf(id+i*2,3,"%02x",random[i]);
    if(mkdirat(root,id,0700)) goto done;
    dir=entry_directory(root,id); if(dir<0) goto done;
    if(manifest(dir,item) || fsync(dir) || fsync(root)) goto done;
    /* Cross-device moves fail before removing anything; no lossy copy fallback. */
    if(renameat2(source,leaf,dir,"payload",RENAME_NOREPLACE)) goto done;
    if(fstatat(dir,"payload",&st,AT_SYMLINK_NOFOLLOW) || !same(&st,item)) {
        /* A source changed between validation and rename. Roll it back if possible.
           If occupied, the payload remains indexed for manual recovery. */
        (void)renameat2(dir,"payload",source,leaf,RENAME_NOREPLACE);
        errno=ESTALE; goto done;
    }
    (void)fsync(source); (void)fsync(dir); result=0;
done:;
    int saved=errno;
    if(dir>=0) {
        if(fstatat(dir,"payload",&st,AT_SYMLINK_NOFOLLOW) && errno==ENOENT) {
            (void)unlinkat(dir,"record",0); close(dir); dir=-1; (void)unlinkat(root,id,AT_REMOVEDIR);
        }
        if(dir>=0) close(dir);
    }
    if(source>=0) close(source);
    close(root); free(leaf); errno=saved; return result;
}
int kl_quarantine_list(const char *store,KList *out)
{
    memset(out,0,sizeof *out);
    int root=private_store(store);
    if(root<0) return errno==ENOENT ? 0 : -1;
    DIR *dir=fdopendir(root); if(!dir) { close(root); return -1; }
    struct dirent *entry; int result=0;
    while((entry=readdir(dir))) {
        if(!id_valid(entry->d_name)) continue;
        int fd=entry_directory(root,entry->d_name); if(fd<0) { out->skipped++; continue; }
        KItem item; struct stat st;
        if(read_manifest(fd,&item)) { out->skipped++; close(fd); continue; }
        if(fstatat(fd,"payload",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) {
            free(item.path); out->skipped++; close(fd); continue;
        }
        close(fd); memcpy(item.id,entry->d_name,33);
        if(append(out,item)) { free(item.path); result=-1; break; }
    }
    closedir(dir); if(result) kl_list_clear(out); return result;
}
static int act(const char *store,const KItem *selected,int release)
{
    int root=private_store(store); if(root<0) return -1;
    int dir=entry_directory(root,selected->id),result=-1;
    if(dir<0) { close(root); return -1; }
    KItem actual={0};
    if(read_manifest(dir,&actual)) goto done;
    if(strcmp(actual.path,selected->path)) { errno=ESTALE; goto done; }
    struct stat st;
    if(fstatat(dir,"payload",&st,AT_SYMLINK_NOFOLLOW) || !same(&st,&actual)) { errno=ESTALE; goto done; }
    if(release) {
        if(unlinkat(dir,"payload",0)) goto done;
    } else {
        char *leaf=NULL; int dest=parent(actual.path,&leaf,1);
        if(dest<0) goto done;
        int moved=renameat2(dir,"payload",dest,leaf,RENAME_NOREPLACE),saved=errno;
        if(!moved) (void)fsync(dest);
        close(dest); free(leaf); errno=saved; if(moved) goto done;
    }
    /* Payload action succeeded. Stale empty metadata is safe if cleanup fails. */
    (void)fsync(dir); (void)unlinkat(dir,"record",0); close(dir); dir=-1;
    (void)unlinkat(root,selected->id,AT_REMOVEDIR); (void)fsync(root); result=0;
done:;
    int saved=errno; free(actual.path); if(dir>=0) close(dir); close(root); errno=saved; return result;
}
int kl_restore(const char *store,const KItem *item) { return act(store,item,0); }
int kl_release(const char *store,const KItem *item) { return act(store,item,1); }
