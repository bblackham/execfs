/* Implementations of all the FUSE operations for this file system. */

/* Use newer version of FUSE API. */
#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "entry.h"
#include "fileops.h"
#include "globals.h"
#include "log.h"

/* All the functions in this file are invoked as FUSE callbacks, which results
 * in assertion failures being invisible to the user. To provide meaningful
 * assertion functionality we provide our own assert() that prints failures to
 * the log.
 */
#undef assert
#ifdef NDEBUG
    #define assert(expr) ((void)0)
#else
    #define assert(expr) do { \
        if (!(expr)) { \
            LOG("%s:%d: %s: Assertion `%s' failed", __FILE__, __LINE__, __func__, #expr); \
            return -1; \
        } \
    } while(0)
#endif

#define BIT(n) (1UL << (n))
#define R BIT(2)
#define W BIT(1)
#define X BIT(0)

#define RIGHTS_MASK 0x3

/* Whether this path is the root of the mount point. */
static int is_root(const char *path) {
    return !strcmp("/", path);
}

/* Note: doing a linear search on the entries array is not an efficient way of
 * implementing a file system that will be under heavy load, but we assume that
 * there will be few entries in the file system and these will not be accessed
 * frequently.
 */
static entry_t *find_entry(const char *path) {
    if (path[0] != '/') {
        /* We were passed a path outside this mount point (?) */
        return NULL;
    }

    int i;
    for (i = 0; i < entries_sz; ++i) {
        if (!strcmp(path + 1, entries[i]->path)) {
            return entries[i];
        }
    }
    return NULL;
}

/* Determine the permissions of a given file in the context of the user
 * currently operating on it.
 */
static unsigned int access_rights(entry_t *entry) {
    struct fuse_context *context = fuse_get_context();
    unsigned int rights;

    if (context->uid == uid) {
        rights = (entry->u_r ? R : 0)
            | (entry->u_w ? W : 0)
            | (entry->u_x ? X : 0);
    } else if (context->gid == gid) {
        rights = (entry->g_r ? R : 0)
            | (entry->g_w ? W : 0)
            | (entry->g_x ? X : 0);
    } else {
        rights = (entry->o_r ? R : 0)
            | (entry->o_w ? W : 0)
            | (entry->o_x ? X : 0);
    }
    return rights;
}

/* Called when the file system is unmounted. */
static void exec_destroy(void *private_data) {
    log_close();
}

static int exec_flush(const char *path, struct fuse_file_info *fi) {
    if (is_root(path)) {
        return 0;
    }

    entry_t *e = find_entry(path);
    if (e == NULL) {
        return -ENOENT;
    }

    assert(fi != NULL);
    assert(fi->fh != 0);
    return fflush((FILE*)fi->fh);
}

static int exec_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    if (is_root(path)) {
        return 0;
    }

    entry_t *e = find_entry(path);
    if (e == NULL) {
        return -ENOENT;
    }

    assert(fi != NULL);
    assert(fi->fh != 0);
    return fsync(fileno((FILE*)fi->fh));
}

/* Start of "interesting" code. The guts of the implementation are below in
 * exec_getattr(), exec_open(), exec_read() and exec_write().
 */

static int exec_getattr(const char *path, struct stat *stbuf) {
    assert(stbuf != NULL);

    /* stbuf->st_dev is ignored. */
    /* stbuf->st_ino is ignored. */

    /* Mark every entry as owned by the mounter. */
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;

    /* stbuf-st_rdev is irrelevant. */
    /* stbuf->st_blksize is ignored. */
    /* stbuf->st_blocks is ignored. */

    /* The current time is as good as any considering any process reading this
     * file may encounter different data to last time.
     */
    stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);

    if (is_root(path)) {
        stbuf->st_mode = S_IFDIR|S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH;
        stbuf->st_size = 0; /* FIXME: This should be set more appropriately. */
        stbuf->st_nlink = 1;
    } else {
        entry_t *e = find_entry(path);
        if (e == NULL) {
            return -ENOENT;
        }

        /* It would be nice to mark entries as FIFOs (S_IFIFO), but
         * irritatingly the kernel doesn't call FUSE handlers for FIFOs so we
         * never get read/write calls.
         */
        stbuf->st_mode = S_IFREG
            | (e->u_r ? S_IRUSR : 0)
            | (e->u_w ? S_IWUSR : 0)
            | (e->u_x ? S_IXUSR : 0)
            | (e->g_r ? S_IRGRP : 0)
            | (e->g_w ? S_IWGRP : 0)
            | (e->g_x ? S_IXGRP : 0)
            | (e->o_r ? S_IROTH : 0)
            | (e->o_w ? S_IWOTH : 0)
            | (e->o_x ? S_IXOTH : 0);
        stbuf->st_size = size;
        stbuf->st_nlink = 1;
    }

    return 0;
}

static int exec_open(const char *path, struct fuse_file_info *fi) {
    entry_t *e = find_entry(path);
    if (e == NULL) {
        return -ENOENT;
    }

    assert(fi != NULL);
    unsigned int entry_rights = access_rights(e);
    unsigned int rights = fi->flags & RIGHTS_MASK;

    if (((rights == O_RDONLY || rights == O_RDWR) && !(entry_rights & R)) ||
        ((rights == O_WRONLY || rights == O_RDWR) && !(entry_rights & W))) {
        return -EACCES;
    }

    LOG("Opening %s (%s) for %s", path, e->command,
        rights == O_RDONLY ? "read" :
        rights == O_WRONLY ? "write" : "read/write");

    /* TODO: rw pipes. */
    fi->fh = (uint64_t)popen(e->command, rights == O_RDONLY ? "r" : "w");
    if (fi->fh == 0) {
        LOG("Failed to popen %s", e->command);
        return -EBADF;
    }
    LOG("Handle %p returned from popen", (FILE*)fi->fh);

    return 0;
}

static int exec_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    assert(fi != NULL);
    assert(fi->fh != 0);

    LOG("read called on %s (popen handle %p)", path, (FILE*)fi->fh);

    size_t sz = fread(buf, 1, size, (FILE*)fi->fh);
    if (sz == -1) {
        LOG("read from %s failed with error %d", path, errno);
    } else {
        LOG("read from %s returned %d bytes", path, sz);
    }
    return sz;
}

static int exec_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    if (!is_root(path)) {
        /* Don't support subdirectories. */
        return -EBADF;
    }

    int i;
    for (i = offset; i < entries_sz; ++i) {
        if (filler(buf, entries[i]->path, NULL, i + 1) != 0) {
            return 0;
        }
    }
    return 0;
}

static int exec_release(const char *path, struct fuse_file_info *fi) {
    assert(is_root(path) || find_entry(path) != NULL);
    assert(fi != NULL);
    assert(fi->fh != 0);
    (void)pclose((FILE*)fi->fh);
    return 0;
}

static int exec_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    assert(fi != NULL);
    assert(fi->fh != 0);

    LOG("write called on %s (popen handle %p)", path, (FILE*)fi->fh);

    size_t sz = fwrite(buf, 1, size, (FILE*)fi->fh);
    if (sz == -1) {
        LOG("write to %s failed with error %d", path, errno);
    } else {
        LOG("write to %s of %d bytes", path, sz);
    }
    return sz;
}

/* Stub out all the irrelevant functions. */
#define FAIL_STUB(func, args...) \
    static int exec_ ## func(const char *path , ## args) { \
        LOG("Fail stubbed function %s called on %s", __func__, path); \
        return -EACCES; \
    }
#define NOP_STUB(func, args...) \
    static int exec_ ## func(const char *path , ## args) { \
        LOG("No-op stubbed function %s called on %s", __func__, path); \
        if (!is_root(path) && find_entry(path) == NULL) { \
            return -ENOENT; \
        } \
        return 0; \
    }
FAIL_STUB(bmap, size_t blocksize, uint64_t *idx);
FAIL_STUB(chmod, mode_t mode); /* Edit the config file to change permissions. */
FAIL_STUB(chown, uid_t uid, gid_t gid);
NOP_STUB(fsyncdir, int datasync, struct fuse_file_info *fi);
FAIL_STUB(link, const char *target);
FAIL_STUB(mkdir, mode_t mode); /* Subdirectories not supported. */
FAIL_STUB(mknod, mode_t mode, dev_t dev);
FAIL_STUB(readlink, char *buf, size_t size); /* Symlinks not supported. */
NOP_STUB(releasedir, struct fuse_file_info *fi);
FAIL_STUB(removexattr, const char *name);
FAIL_STUB(rename, const char *new_name);
FAIL_STUB(rmdir);
FAIL_STUB(setxattr, const char *name, const char *value, size_t size, int flags);
FAIL_STUB(symlink, const char *target);
NOP_STUB(truncate, off_t size);
FAIL_STUB(unlink); /* Edit the config file to remove entries. */
NOP_STUB(utime, struct utimbuf *buf);
NOP_STUB(utimens, const struct timespec tv[2]);
#undef FAIL_STUB
#undef NOP_STUB

#define OP(func) .func = &exec_ ## func
struct fuse_operations ops = {
    .flag_nullpath_ok = 0, /* Don't accept NULL paths. */
    // TODO access
    OP(bmap),
    OP(chmod),
    OP(chown),
    /* No need to implement create as open gets be called instead. */
    OP(destroy),
    // TODO fgetattr
    OP(flush),
    /* No need to implement ftruncate as truncate gets called instead. */
    OP(fsync),
    OP(fsyncdir),
    OP(getattr),
    // TODO getxattr
    // TODO init
    // TODO ioctl
    OP(link),
    // TODO listxattr
    /* No need to implement lock. Let the kernel handle flocking. */
    OP(mkdir),
    OP(mknod),
    OP(open),
    // TODO opendir
    // TODO poll
    OP(read),
    OP(readdir),
    OP(readlink),
    OP(release),
    OP(releasedir),
    OP(removexattr),
    OP(rename),
    OP(rmdir),
    OP(setxattr),
    // TODO statfs
    OP(symlink),
    OP(truncate),
    OP(unlink),
    OP(utime),
    OP(utimens),
    OP(write),
};
#undef OP
