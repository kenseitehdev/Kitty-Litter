#define _GNU_SOURCE
#include "cleanup.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static void path(char *out,const char *root,const char *suffix)
{ assert(snprintf(out,PATH_MAX,"%s/%s",root,suffix)>0); }
static void file(const char *p,off_t size,int days)
{
    int fd=open(p,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0); assert(!ftruncate(fd,size));
    struct timespec times[2]={{time(NULL)-(time_t)days*86400,0},{time(NULL)-(time_t)days*86400,0}};
    assert(!futimens(fd,times)); close(fd);
}
int main(int argc,char **argv)
{
    assert(argc==2); const char *root=argv[1];
    char cache[PATH_MAX],logs[PATH_MAX],downloads[PATH_MAX],store[PATH_MAX],p[PATH_MAX],q[PATH_MAX];
    path(cache,root,"cache"); path(logs,root,"logs"); path(downloads,root,"downloads"); path(store,root,"data/quarantine");
    assert(!mkdir(cache,0700)); assert(!mkdir(logs,0700)); assert(!mkdir(downloads,0700));
    path(p,cache,"old-cache"); file(p,1048576,40);
    path(p,cache,"recent"); file(p,1048576,2);
    path(p,cache,"small"); file(p,12,40);
    path(p,cache,".hidden"); file(p,1048576,40);
    path(p,logs,"old-log"); file(p,1048576*2,40);
    path(p,downloads,"archive.zip"); file(p,1048576*50,70);
    path(p,downloads,"paper.pdf"); file(p,1048576*50,70);
    path(p,cache,"link"); path(q,downloads,"archive.zip"); assert(!symlink(q,p));
    path(p,cache,"dir-link"); assert(!symlink(downloads,p));
    KList candidates={0};
    assert(!kl_scan(cache,logs,downloads,time(NULL),&candidates));
    assert(candidates.count==3); assert(candidates.items[0].bytes==1048576*50);
    assert(!kl_store_prepare(store));
    assert(!kl_quarantine(store,&candidates.items[0]));
    assert(access(candidates.items[0].path,F_OK)<0);
    KList records={0}; assert(!kl_quarantine_list(store,&records)); assert(records.count==1);
    /* Existing destinations, including dangling symlinks, must never be replaced. */
    assert(!symlink("missing",records.items[0].path));
    assert(kl_restore(store,&records.items[0])<0 && errno==EEXIST);
    assert(!unlink(records.items[0].path));
    assert(!kl_restore(store,&records.items[0])); assert(!access(candidates.items[0].path,F_OK));
    kl_list_clear(&records);
    /* Same item can be quarantined again; inode and timestamp survive rename. */
    assert(!kl_quarantine(store,&candidates.items[0]));
    assert(!kl_quarantine_list(store,&records)); assert(records.count==1);
    assert(!kl_release(store,&records.items[0])); kl_list_clear(&records);
    assert(!kl_quarantine_list(store,&records)); assert(records.count==0); kl_list_clear(&records);
    /* Reject changes after the review scan. */
    int fd=open(candidates.items[1].path,O_WRONLY|O_APPEND); assert(fd>=0); assert(write(fd,"changed",7)==7); close(fd);
    assert(kl_quarantine(store,&candidates.items[1])<0 && errno==ESTALE);
    /* Do not follow a replaced file or a symlink in any parent directory. */
    assert(!unlink(candidates.items[2].path)); assert(!symlink(q,candidates.items[2].path));
    assert(kl_quarantine(store,&candidates.items[2])<0 && errno==ESTALE);
    kl_list_clear(&candidates);
    assert(!kl_scan(cache,cache,downloads,time(NULL),&candidates));
    for(size_t i=0;i<candidates.count;++i) for(size_t j=i+1;j<candidates.count;++j)
        assert(strcmp(candidates.items[i].path,candidates.items[j].path));
    kl_list_clear(&candidates);
    /* Refuse insecure storage and directory symlink traversal. */
    assert(!chmod(store,0777)); assert(kl_store_prepare(store)<0 && errno==EACCES); assert(!chmod(store,0700));
    path(p,root,"store-link"); assert(!symlink(store,p)); assert(kl_store_prepare(p)<0);
    /* Interrupted operation: a record with missing payload is reported, not acted on. */
    path(p,store,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); assert(!mkdir(p,0700));
    assert(!kl_quarantine_list(store,&records)); assert(!records.count && records.skipped==1); kl_list_clear(&records);
    /* Restore missing parents, and reject a symlink replacing a parent. */
    path(p,logs,"nested"); assert(!mkdir(p,0700));
    char nested[PATH_MAX]; path(nested,p,"old nested.log"); file(nested,1048576,40);
    assert(!kl_scan(cache,logs,downloads,time(NULL),&candidates));
    size_t found=0;
    while(found<candidates.count && strcmp(candidates.items[found].path,nested)) ++found;
    assert(found<candidates.count); assert(!kl_quarantine(store,&candidates.items[found]));
    assert(!rmdir(p)); assert(!kl_quarantine_list(store,&records)); assert(records.count==1);
    assert(!kl_restore(store,&records.items[0])); kl_list_clear(&records);
    assert(!access(nested,F_OK));
    assert(!kl_quarantine(store,&candidates.items[found])); assert(!rmdir(p));
    char external[PATH_MAX]; path(external,root,"outside"); assert(!mkdir(external,0700));
    assert(!symlink(external,p)); assert(!kl_quarantine_list(store,&records)); assert(records.count==1);
    assert(kl_restore(store,&records.items[0])<0);
    char outside_file[PATH_MAX]; path(outside_file,external,"old nested.log"); assert(access(outside_file,F_OK)<0);
    assert(!unlink(p)); assert(!mkdir(p,0700));
    /* Mutated quarantined payloads are neither restored nor deleted. */
    char payload[PATH_MAX]; assert(snprintf(payload,sizeof payload,"%s/%s/payload",store,records.items[0].id)>0);
    fd=open(payload,O_WRONLY|O_APPEND); assert(fd>=0); assert(write(fd,"x",1)==1); close(fd);
    assert(kl_restore(store,&records.items[0])<0 && errno==ESTALE);
    assert(kl_release(store,&records.items[0])<0 && errno==ESTALE); assert(!access(payload,F_OK));
    kl_list_clear(&candidates); kl_list_clear(&records);
    puts("Scan, thresholds, symlink exclusion, quarantine, restore conflicts, permanent release, stale-source, duplicate-root, and incomplete-record checks passed.");
    return 0;
}
