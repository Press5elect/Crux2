// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/* Crux2 - name lookups, and the libc functions the console lacks (or may refuse)
 * that the emulator and its libraries call.
 *
 * The console's libc has no getaddrinfo a title can use (the platform layer's refuses every
 * lookup). libsmb2 and curl both go through it, even for a numeric address, so the title has its
 * own: an IPv4 address is parsed; a name is asked of the console's resolver (libSceNet), which is
 * what the cover and achievement downloads need. The link binds libc's names to these
 * (ps5/tools/link-title.sh), kept local to the title: every function here is ps5px_<libc name>.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* libSceNet; no SDK header declares these. */
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address, int timeout,
                            int retries, int flags);
int sceNetResolverDestroy(int resolver);

void ps5px_log(const char *format, ...);

static int
resolve_name(const char *name, struct in_addr *address)
{
   static int initialised;
   if (!initialised) {
      (void)sceNetInit();
      initialised = 1;
   }

   const int pool = sceNetPoolCreate("ps5px_resolver", 64 * 1024, 0);
   if (pool < 0) {
      ps5px_log("resolver: sceNetPoolCreate failed (%#x)", (unsigned)pool);
      return EAI_FAIL;
   }

   int result = EAI_FAIL;
   const int resolver = sceNetResolverCreate("ps5px_resolver", pool, 0);
   if (resolver < 0) {
      ps5px_log("resolver: sceNetResolverCreate failed (%#x)", (unsigned)resolver);
   } else {
      const int rc = sceNetResolverStartNtoa(resolver, name, address, 0, 0, 0);
      if (rc < 0)
         ps5px_log("resolver: %s was not found (%#x)", name, (unsigned)rc);
      else
         result = 0;
      (void)sceNetResolverDestroy(resolver);
   }
   (void)sceNetPoolDestroy(pool);
   return result == 0 ? 0 : EAI_NONAME;
}

int
ps5px_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                  struct addrinfo **result)
{
   if (!result)
      return EAI_FAIL;
   *result = NULL;
   if (!node && !service)
      return EAI_NONAME;

   const int flags = hints ? hints->ai_flags : 0;
   const int family = hints ? hints->ai_family : AF_UNSPEC;
   if (family != AF_UNSPEC && family != AF_INET)
      return EAI_FAMILY; /* the title speaks IPv4 only */

   int socktype = hints ? hints->ai_socktype : 0;
   int protocol = hints ? hints->ai_protocol : 0;
   if (socktype == 0)
      socktype = SOCK_STREAM;
   if (protocol == 0)
      protocol = (socktype == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;

   long port = 0;
   if (service && service[0]) {
      char *end = NULL;
      port = strtol(service, &end, 10);
      if (!end || *end != '\0') {
         if (strcmp(service, "http") == 0)
            port = 80;
         else if (strcmp(service, "https") == 0)
            port = 443;
         else if (strcmp(service, "microsoft-ds") == 0)
            port = 445;
         else
            return EAI_SERVICE;
      }
      if (port < 0 || port > 65535)
         return EAI_SERVICE;
   }

   struct in_addr address;
   memset(&address, 0, sizeof(address));
   if (!node) {
      address.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
   } else if (inet_pton(AF_INET, node, &address) != 1) {
      if (flags & AI_NUMERICHOST)
         return EAI_NONAME;
      const int error = resolve_name(node, &address);
      if (error != 0)
         return error;
   }

   /* One allocation: the entry, then its address. */
   struct addrinfo *info = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
   if (!info)
      return EAI_MEMORY;
   struct sockaddr_in *sin = (struct sockaddr_in *)(info + 1);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
   sin->sin_len = sizeof(*sin);
#endif
   sin->sin_family = AF_INET;
   sin->sin_port = htons((uint16_t)port);
   sin->sin_addr = address;
   info->ai_flags = flags;
   info->ai_family = AF_INET;
   info->ai_socktype = socktype;
   info->ai_protocol = protocol;
   info->ai_addrlen = sizeof(*sin);
   info->ai_addr = (struct sockaddr *)sin;
   if ((flags & AI_CANONNAME) && node)
      info->ai_canonname = strdup(node);
   *result = info;
   return 0;
}

void
ps5px_freeaddrinfo(struct addrinfo *info)
{
   while (info) {
      struct addrinfo *const next = info->ai_next;
      free(info->ai_canonname);
      free(info);
      info = next;
   }
}

/* ---- the rest: small libc functions no system module exports -------------------------- */

/* pipe2: curl's wake-up pipe. */
int
ps5px_pipe2(int fds[2], int flags)
{
   if (pipe(fds) != 0)
      return -1;
   for (int i = 0; i < 2; i++) {
      if (flags & O_NONBLOCK)
         (void)fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
      if (flags & O_CLOEXEC)
         (void)fcntl(fds[i], F_SETFD, FD_CLOEXEC);
   }
   return 0;
}

/* lockf: the emulator locks its cache files against a second instance; a title runs once. */
int
ps5px_lockf(int fd, int function, off_t size)
{
   (void)fd;
   (void)function;
   (void)size;
   return 0;
}

long
ps5px_gethostid(void)
{
   return 0;
}

/* timegm: struct tm in UTC to seconds since 1970 (days from the civil date, as in
 * Howard Hinnant's algorithm). */
time_t
ps5px_timegm(struct tm *tm)
{
   long long year = (long long)tm->tm_year + 1900;
   int month = tm->tm_mon; /* 0..11, may be out of range as mktime allows */
   year += month / 12;
   month %= 12;
   if (month < 0) {
      month += 12;
      year--;
   }
   month += 1;

   /* The first of the month, then the day of the month added as it is: it may be out of range
    * too (day 0 is the last day of the month before). */
   year -= month <= 2;
   const long long era = (year >= 0 ? year : year - 399) / 400;
   const unsigned yoe = (unsigned)(year - era * 400);
   const unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u;
   const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
   const long long days = era * 146097LL + (long long)doe - 719468LL + ((long long)tm->tm_mday - 1);
   return (time_t)(days * 86400LL + (long long)tm->tm_hour * 3600LL + (long long)tm->tm_min * 60LL + tm->tm_sec);
}

/* getprotobyname: libsmb2 asks for "tcp" to set TCP_NODELAY. */
struct protoent *
ps5px_getprotobyname(const char *name)
{
   static char tcp_name[] = "tcp";
   static char udp_name[] = "udp";
   static char *no_aliases[] = {NULL};
   static struct protoent tcp = {tcp_name, no_aliases, IPPROTO_TCP};
   static struct protoent udp = {udp_name, no_aliases, IPPROTO_UDP};
   if (name && strcmp(name, "tcp") == 0)
      return &tcp;
   if (name && strcmp(name, "udp") == 0)
      return &udp;
   return NULL;
}

/* sendmmsg, recvmmsg: OpenSSL's datagram BIO (DTLS, QUIC) names them; nothing here uses datagram
 * TLS, and the console has neither call. */
int
ps5px_sendmmsg(int fd, void *messages, unsigned count, int flags)
{
   (void)fd;
   (void)messages;
   (void)count;
   (void)flags;
   errno = ENOSYS;
   return -1;
}

int
ps5px_recvmmsg(int fd, void *messages, unsigned count, int flags, void *timeout)
{
   (void)fd;
   (void)messages;
   (void)count;
   (void)flags;
   (void)timeout;
   errno = ENOSYS;
   return -1;
}

/* _setjmp, _longjmp: OpenSSL's asynchronous jobs switch fibres with them. Nothing here starts an
 * asynchronous job (it takes SSL_MODE_ASYNC, which curl does not set); the console's libc has only
 * the plain names, and a function cannot stand in for setjmp. Should a job ever be started, the
 * title stops with a message rather than jump through a context that was never saved. */
void ps5_title_fatal(const char *message) __attribute__((noreturn));

int
ps5px__setjmp(void *environment)
{
   (void)environment;
   return 0;
}

void
ps5px__longjmp(void *environment, int value)
{
   (void)environment;
   (void)value;
   ps5_title_fatal("OpenSSL started an asynchronous job, which this title does not support.");
}

/* ---- calls the stub libraries accept, taken out of the first console run --------------------
 *
 * Each of these is on a path the title really runs (opening a share, the first HTTPS download, a
 * database commit), and is either a system call a title may be refused or a function exported by
 * a module a title should not depend on (libkernel_sys, libScePosixForWebKit). The answers below
 * are the ones that are true on the console, so nothing is asked of it. */

/* getlogin_r, gethostname: libsmb2 fills in a default user and workstation name when a context is
 * made; the port always sets the user itself. */
int
ps5px_getlogin_r(char *name, int length)
{
   if (!name || length < 6)
      return ERANGE;
   memcpy(name, "guest", 6);
   return 0;
}

int
ps5px_gethostname(char *name, size_t length)
{
   if (!name || length < 4) {
      errno = EINVAL;
      return -1;
   }
   memcpy(name, "PS5", 4);
   return 0;
}

/* random, srandom: libsmb2's client GUID and message salts. The platform layer's arc4random is
 * the stronger source, and needs no seed. */
uint32_t ps5_arc4random(void);

long
ps5px_random(void)
{
   return (long)(ps5_arc4random() & 0x7fffffffu);
}

void
ps5px_srandom(unsigned seed)
{
   (void)seed;
}

/* fdatasync: SQLite ends each commit with it (the achievement and download caches). */
int
ps5px_fdatasync(int fd)
{
   return fsync(fd);
}

/* issetugid: OpenSSL asks before it reads an environment variable. A title is neither. */
int
ps5px_issetugid(void)
{
   return 0;
}

/* pathconf: libc++'s std::filesystem names it (libkernel_sys only). */
long
ps5px_pathconf(const char *path, int name)
{
   (void)path;
   switch (name) {
   case _PC_NAME_MAX:
      return 255;
   case _PC_PATH_MAX:
      return 1024;
   case _PC_LINK_MAX:
      return 1;
   default:
      errno = EINVAL;
      return -1;
   }
}

/* memrchr: curl's URL parser (libSceLibcInternal only). */
void *
ps5px_memrchr(const void *memory, int value, size_t length)
{
   const unsigned char *const bytes = memory;
   while (length-- > 0) {
      if (bytes[length] == (unsigned char)value)
         return (void *)(bytes + length);
   }
   return NULL;
}

/* basename: curl's MIME parts (libSceLibcInternal only). POSIX's: may return static storage. */
char *
ps5px_basename(char *path)
{
   static char dot[] = ".";
   static char result[1024];
   if (!path || !path[0])
      return dot;
   size_t end = strlen(path);
   while (end > 1 && path[end - 1] == '/')
      end--;
   size_t start = end;
   while (start > 0 && path[start - 1] != '/')
      start--;
   if (end == 1 && path[0] == '/')
      start = 0;
   size_t length = end - start;
   if (length >= sizeof(result))
      length = sizeof(result) - 1;
   memcpy(result, path + start, length);
   result[length] = '\0';
   return result;
}

/* fnmatch: curl's FTP wildcards (libScePosixForWebKit only). '*', '?' and literal characters;
 * no bracket expressions, which nothing here passes. Returns 0 on a match, 1 (FNM_NOMATCH) else. */
int
ps5px_fnmatch(const char *pattern, const char *string, int flags)
{
   (void)flags;
   const char *star = NULL, *resume = NULL;
   while (*string) {
      if (*pattern == '*') {
         star = pattern++;
         resume = string;
      } else if (*pattern == '?' || *pattern == *string) {
         pattern++;
         string++;
      } else if (star) {
         pattern = star + 1;
         string = ++resume;
      } else {
         return 1;
      }
   }
   while (*pattern == '*')
      pattern++;
   return *pattern ? 1 : 0;
}

/* recvmsg: the emulated network adapter's ICMP session reads one datagram into one buffer with it.
 * recvfrom does the same and is among the calls a title is known to have. A message of several
 * buffers, or one that wants control data, is not what this is for. */
ssize_t
ps5px_recvmsg(int socket_fd, struct msghdr *message, int flags)
{
   if (!message || message->msg_iovlen != 1 || !message->msg_iov) {
      errno = EINVAL;
      return -1;
   }

   socklen_t name_length = message->msg_name ? message->msg_namelen : 0;
   const ssize_t result = recvfrom(socket_fd, message->msg_iov[0].iov_base, message->msg_iov[0].iov_len, flags,
                                   (struct sockaddr *)message->msg_name, message->msg_name ? &name_length : NULL);
   if (result >= 0) {
      message->msg_namelen = name_length;
      message->msg_controllen = 0;
      message->msg_flags = 0;
   }
   return result;
}

#ifdef __PROSPERO__
#include <sys/stat.h>

/* lstat: inside the title's sandbox it fails where stat answers (the first console runs with USB
 * drives off: every "is this a link?" question failed, so the emulator saw no BIOS image, no file
 * in a folder it listed, and SQLite could not open its database). The title folder has no links a
 * title could follow anyway: when lstat fails for a reason other than the file not being there,
 * stat's answer stands in. The link routes every lstat in the title here (--wrap). */
extern int __real_lstat(const char *path, struct stat *status);
extern void ps5px_log(const char *format, ...);

int
__wrap_lstat(const char *path, struct stat *status)
{
   const int result = __real_lstat(path, status);
   if (result == 0 || errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG)
      return result;

   const int reason = errno;
   static int reported;
   if (!reported) {
      reported = 1;
      ps5px_log("lstat(%s) failed (errno %d): stat answers for it from here on", path, reason);
   }
   if (stat(path, status) == 0)
      return 0;
   errno = reason;
   return -1;
}

#include <poll.h>

/* poll: a signal ends it with EINTR, and SA_RESTART does not start it again (sleeps and waits are
 * never restarted). The sampling profiler signals the threads it samples every 2 ms, and a thread
 * that waits on a network share then saw its wait fail: libsmb2 took the EINTR for a lost
 * connection ("Poll failed", then errno 65 when connecting again), and a game on the share could
 * not start. Every poll in the title comes here (--wrap): an interrupted one waits again for what
 * is left of its time. */
extern int __real_poll(struct pollfd *fds, nfds_t count, int timeout_ms);

int
__wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
   long long deadline_ns = 0;
   if (timeout_ms > 0) {
      struct timespec start;
      clock_gettime(CLOCK_MONOTONIC, &start);
      deadline_ns = (long long)start.tv_sec * 1000000000LL + start.tv_nsec + (long long)timeout_ms * 1000000LL;
   }
   int wait_ms = timeout_ms;
   for (;;) {
      const int result = __real_poll(fds, count, wait_ms);
      if (result >= 0 || errno != EINTR)
         return result;
      if (timeout_ms > 0) {
         struct timespec now;
         clock_gettime(CLOCK_MONOTONIC, &now);
         const long long left_ns = deadline_ns - ((long long)now.tv_sec * 1000000000LL + now.tv_nsec);
         if (left_ns <= 0)
            return 0; /* the time is up, as an uninterrupted poll would have said */
         wait_ms = (int)((left_ns + 999999LL) / 1000000LL);
      }
   }
}
#endif
