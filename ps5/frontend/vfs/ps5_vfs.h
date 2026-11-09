// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - the virtual file layer: network shares and sandbox-hidden
 * drives as ordinary paths.
 *
 * A path under a mount point (/smb/<name>/... for a network share, SMB or NFS
 * alike, or /mnt/... when USB drives are only reachable through the console's FTP
 * server) is served by a backend; every other path goes to libc untouched.
 * PCSX2's stdio calls are bound to the
 * ps5vfs_f* functions below by the build (compat/ps5_stdio_redirect.cmake), and its
 * file_system.cpp asks ps5vfs_stat / ps5vfs_opendir for virtual paths, so game
 * lists, disc images, CHD parents and cue sheets on a share work as local files do.
 * Virtual files are read-only through stdio; whole files are written with ps5vfs_store, which
 * is how the backups of memory cards and saved states reach a share.
 */
#ifndef PS5PX_VFS_H
#define PS5PX_VFS_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- mounts ---------------------------------------------------------------- */

/* An SMB2/3 share at /smb/<name>. url is smb://[domain;][user[:password]@]host[:port]/share[/folder].
 * The connection is made on first use. Returns 0, or -1 for a malformed name or URL. */
int ps5vfs_mount_smb(const char* name, const char* url);

/* An NFS export (or a folder inside one) at /smb/<name>, beside the SMB shares: the interface lists
 * every /smb mount as a share. url is
 *
 *     nfs://server[:nfsport]/path[?uid=N&gid=N&version=3|4|4.2&nfsport=N&mountport=N]
 *
 * path is the folder on the server (/volume1/games/ps2); with NFS 3 the server's list of exports
 * says which part of it is the export. version: 3 by default, with NFS 4 tried when 3 fails; 4 or
 * 4.2 for NFS 4 only. uid and gid: the user and group the server is told (AUTH_SYS), 1000 and 1000
 * by default. nfsport and mountport: the servers' ports, in place of the portmapper's answer. The
 * connection is made on first use. Returns 0, or -1 for a malformed name or URL. */
int ps5vfs_mount_nfs(const char* name, const char* url);

/* The folder remote_root of an FTP server on 127.0.0.1:port, at mount_point
 * (for /mnt, which a title's sandbox hides). Returns 0, or -1 when the server does not answer. */
int ps5vfs_mount_ftp(const char* mount_point, unsigned port, const char* remote_root);

/* Reads network.cfg:
 *
 *     path = 192.168.1.10/Games/PS2
 *     path = nfs://192.0.2.20/volume1/games/ps2
 *     user = guest
 *     password =
 *     uid = 1000
 *     gid = 1000
 *
 * Each path line is one share, shown as /smb/<its last folder>. A path is SMB unless it begins with
 * nfs://: server/share/folder (the server by its IP address; \\server\share and smb:// are the same),
 * with user and password the account for all SMB shares ("guest" and an empty password for an open
 * share). An nfs:// path is an NFS export or a folder in one (ps5vfs_mount_nfs), with uid and gid
 * the user and group numbers given to every NFS server whose path does not name its own (1000 and
 * 1000 when the file has none); user and password are not used for NFS. # and ; start comments.
 * Returns the number of shares mounted, or -1 when the file cannot be read. */
int ps5vfs_load_network_config(const char* config_path);

/* The mount points, one per call: index 0.. until NULL. */
const char* ps5vfs_mount_point(unsigned index);

/* One line per mount ("<mount point>: <what it is>, <state>") into buffer; returns its length. */
int ps5vfs_describe(char* buffer, size_t size);

/* Where the layer's messages go (connection failures, mounts). NULL: nowhere. */
void ps5vfs_set_log(void (*log)(const char* line));

/* The real file whose FILE stands for each virtual stream (it is opened for reading and never
 * read from): one that is always there. The string must stay valid. The default is the title's
 * /app0/sce_sys/param.json, which a title that left its sandbox no longer finds under that name. */
void ps5vfs_set_shadow_path(const char* path);

/* ---- paths ----------------------------------------------------------------- */

/* Whether a path is served by this layer (it lies at or under a mount point, or is the
 * parent folder of mount points, such as /smb). */
int ps5vfs_is_virtual(const char* path);

/* stat() for a virtual path: 0, or -1 with errno. st_mode (S_IFDIR or S_IFREG), st_size and the
 * times are filled. */
int ps5vfs_stat(const char* path, struct stat* st);

/* fstat() for a virtual stream. Returns 1 when fp is virtual and st was filled, 0 when it is
 * not virtual (the caller asks libc), -1 with errno on failure. */
int ps5vfs_fstat(FILE* fp, struct stat* st);

struct ps5vfs_dirent
{
  const char* name; /* valid until the next call on the same directory */
  int is_directory;
  int64_t size;
  int64_t mtime;
};
typedef struct ps5vfs_dir ps5vfs_dir;
ps5vfs_dir* ps5vfs_opendir(const char* path);
int ps5vfs_readdir(ps5vfs_dir* dir, struct ps5vfs_dirent* entry); /* 1: an entry, 0: the end */
void ps5vfs_closedir(ps5vfs_dir* dir);

/* ---- stdio ----------------------------------------------------------------- */

FILE* ps5vfs_fopen(const char* path, const char* mode);
int ps5vfs_fclose(FILE* fp);
size_t ps5vfs_fread(void* buffer, size_t size, size_t count, FILE* fp);
size_t ps5vfs_fwrite(const void* buffer, size_t size, size_t count, FILE* fp);
int ps5vfs_fseek(FILE* fp, long offset, int whence);
int ps5vfs_fseeko(FILE* fp, off_t offset, int whence);
long ps5vfs_ftell(FILE* fp);
off_t ps5vfs_ftello(FILE* fp);
void ps5vfs_rewind(FILE* fp);
int ps5vfs_feof(FILE* fp);
int ps5vfs_ferror(FILE* fp);
void ps5vfs_clearerr(FILE* fp);
char* ps5vfs_fgets(char* buffer, int size, FILE* fp);
int ps5vfs_fgetc(FILE* fp);
int ps5vfs_ungetc(int c, FILE* fp);
int ps5vfs_fflush(FILE* fp);
int ps5vfs_fileno(FILE* fp);

/* ---- writing ---------------------------------------------------------------
 * Whole files only, and only for virtual paths. Each returns 0, or -1 with errno (EROFS from a
 * mount that cannot write, EACCES when the server refuses the account). */

/* Makes the folder and those above it, below the mount point. One that is there is left alone. */
int ps5vfs_make_folders(const char* path);
/* Writes the file: beside its place first, then moved there in place of what was there. */
int ps5vfs_store(const char* path, const void* data, size_t size);
/* Takes a file (folder 0) or an empty folder (folder 1) away. One that is not there is no error. */
int ps5vfs_remove(const char* path, int folder);

/* Counters, for a report: virtual streams open now, bytes read through backends, backend reads. */
void ps5vfs_stats(unsigned* open_files, uint64_t* bytes_read, uint64_t* backend_reads);

#ifdef __cplusplus
}

#include <functional>

/* ps5vfs_store for a file too large to hold in memory (a disc image): `size` bytes, asked of
 * `source` piece by piece (source(buffer, length, offset) fills the buffer and returns length, or
 * -1 with errno to stop; it may be asked again from 0 after a lost connection). Returns 0, or -1
 * with errno; a stopped or failed transfer leaves no file under the name. */
int ps5vfs_store_from(const char* path, uint64_t size, const std::function<int64_t(void* buffer, uint32_t length, uint64_t offset)>& source);
#endif

#endif /* PS5PX_VFS_H */
