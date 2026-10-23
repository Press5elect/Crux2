// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/* Crux2 - host test of the libc stand-ins (frontend/ps5_libc.c), each against the
 * PC's own libc where it has the function, with the console's resolver replaced by a fake. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

int ps5px_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result);
void ps5px_freeaddrinfo(struct addrinfo *info);
int ps5px_pipe2(int fds[2], int flags);
time_t ps5px_timegm(struct tm *tm);
struct protoent *ps5px_getprotobyname(const char *name);
int ps5px_getlogin_r(char *name, int length);
int ps5px_gethostname(char *name, size_t length);
long ps5px_random(void);
long ps5px_pathconf(const char *path, int name);
void *ps5px_memrchr(const void *memory, int value, size_t length);
char *ps5px_basename(char *path);
int ps5px_fnmatch(const char *pattern, const char *string, int flags);
ssize_t ps5px_recvmsg(int socket_fd, struct msghdr *message, int flags);

static int checks, failures;
#define CHECK(condition)                                                                           \
   do {                                                                                            \
      checks++;                                                                                    \
      if (!(condition)) {                                                                          \
         failures++;                                                                               \
         fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                    \
      }                                                                                            \
   } while (0)

/* ---- what the console provides, faked ---- */
static int resolver_calls;
int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char *name, int size, int flags) { (void)name; (void)size; (void)flags; return 7; }
int sceNetPoolDestroy(int pool) { return pool == 7 ? 0 : -1; }
int sceNetResolverCreate(const char *name, int pool, int flags) { (void)name; (void)flags; return pool == 7 ? 9 : -1; }
int sceNetResolverDestroy(int resolver) { return resolver == 9 ? 0 : -1; }
int
sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address, int timeout, int retries, int flags)
{
   (void)timeout; (void)retries; (void)flags;
   resolver_calls++;
   if (resolver != 9 || strcmp(hostname, "covers.example") != 0)
      return (int)0x804101e2;
   inet_pton(AF_INET, "203.0.113.7", address);
   return 0;
}
void ps5px_log(const char *format, ...) { (void)format; }
uint32_t ps5_arc4random(void) { return (uint32_t)rand() * 2654435761u; }
void ps5_title_fatal(const char *message) { fprintf(stderr, "fatal: %s\n", message); exit(3); }

static const char *
address_of(const struct addrinfo *info, int *port)
{
   static char text[64];
   const struct sockaddr_in *sin = (const struct sockaddr_in *)info->ai_addr;
   *port = ntohs(sin->sin_port);
   return inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
}

static void
test_getaddrinfo(void)
{
   struct addrinfo *info = NULL;
   int port = 0;

   /* How libsmb2 asks: a numeric address, a numeric port, no hints. */
   CHECK(ps5px_getaddrinfo("192.0.2.1", "445", NULL, &info) == 0 && info);
   if (info) {
      CHECK(info->ai_family == AF_INET && info->ai_socktype == SOCK_STREAM && info->ai_protocol == IPPROTO_TCP);
      CHECK(info->ai_addrlen == sizeof(struct sockaddr_in) && info->ai_next == NULL);
      CHECK(strcmp(address_of(info, &port), "192.0.2.1") == 0 && port == 445);
      CHECK(resolver_calls == 0);
      ps5px_freeaddrinfo(info);
   }

   /* How curl asks: hints with a socket type, the port as a number. */
   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   CHECK(ps5px_getaddrinfo("covers.example", "443", &hints, &info) == 0 && info);
   if (info) {
      CHECK(strcmp(address_of(info, &port), "203.0.113.7") == 0 && port == 443);
      CHECK(resolver_calls == 1);
      ps5px_freeaddrinfo(info);
   }

   CHECK(ps5px_getaddrinfo("no-such-host.example", "80", &hints, &info) == EAI_NONAME && info == NULL);
   hints.ai_flags = AI_NUMERICHOST;
   const int calls = resolver_calls;
   CHECK(ps5px_getaddrinfo("covers.example", "80", &hints, &info) == EAI_NONAME && resolver_calls == calls);
   hints.ai_flags = 0;
   hints.ai_family = AF_INET6;
   CHECK(ps5px_getaddrinfo("192.0.2.1", "80", &hints, &info) == EAI_FAMILY);
   hints.ai_family = AF_INET;
   CHECK(ps5px_getaddrinfo("192.0.2.1", "https", &hints, &info) == 0 && info);
   if (info) {
      (void)address_of(info, &port);
      CHECK(port == 443);
      ps5px_freeaddrinfo(info);
   }
   CHECK(ps5px_getaddrinfo("192.0.2.1", "gopher", &hints, &info) == EAI_SERVICE);
   CHECK(ps5px_getaddrinfo("192.0.2.1", "70000", &hints, &info) == EAI_SERVICE);
   CHECK(ps5px_getaddrinfo(NULL, NULL, &hints, &info) == EAI_NONAME);
   hints.ai_flags = AI_PASSIVE;
   CHECK(ps5px_getaddrinfo(NULL, "8080", &hints, &info) == 0 && info);
   if (info) {
      CHECK(strcmp(address_of(info, &port), "0.0.0.0") == 0 && port == 8080);
      ps5px_freeaddrinfo(info);
   }
   hints.ai_flags = AI_CANONNAME;
   hints.ai_socktype = SOCK_DGRAM;
   CHECK(ps5px_getaddrinfo("10.0.0.1", NULL, &hints, &info) == 0 && info);
   if (info) {
      CHECK(info->ai_socktype == SOCK_DGRAM && info->ai_protocol == IPPROTO_UDP);
      CHECK(info->ai_canonname && strcmp(info->ai_canonname, "10.0.0.1") == 0);
      ps5px_freeaddrinfo(info);
   }
   ps5px_freeaddrinfo(NULL);
}

static void
test_timegm(void)
{
   srand(12345);
   int mismatches = 0;
   for (int i = 0; i < 200000; i++) {
      struct tm tm;
      memset(&tm, 0, sizeof(tm));
      tm.tm_year = (rand() % 400) - 100; /* 1800..2199 */
      tm.tm_mon = (rand() % 30) - 9;     /* out of range too, as mktime allows */
      tm.tm_mday = (rand() % 40) - 4;
      tm.tm_hour = rand() % 24;
      tm.tm_min = rand() % 60;
      tm.tm_sec = rand() % 61;
      struct tm copy = tm;
      mismatches += (ps5px_timegm(&tm) != timegm(&copy));
   }
   CHECK(mismatches == 0);
   struct tm epoch;
   memset(&epoch, 0, sizeof(epoch));
   epoch.tm_year = 70;
   epoch.tm_mday = 1;
   CHECK(ps5px_timegm(&epoch) == 0);
}

static void
test_strings(void)
{
   static const char *const patterns[] = {"*", "*.chd", "game?.bin", "a*b*c", "", "?", "**x", "Final*(Disc ?)*", "abc"};
   static const char *const strings[] = {"", "x", "abc", "game1.bin", "game10.bin", "Crash.chd", "axxbyyc", "abcabc",
                                         "Final Fantasy VII (Disc 2).chd", ".chd", "xx"};
   int mismatches = 0;
   for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
      for (size_t s = 0; s < sizeof(strings) / sizeof(strings[0]); s++)
         mismatches += ((ps5px_fnmatch(patterns[p], strings[s], 0) == 0) != (fnmatch(patterns[p], strings[s], 0) == 0));
   }
   CHECK(mismatches == 0);

   static const char *const paths[] = {"/usr/lib", "/usr/", "usr", "/", ".", "..", "a/b/c.txt", "c.txt", "//", "a//b//"};
   for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
      char mine[64], theirs[64];
      strcpy(mine, paths[i]);
      strcpy(theirs, paths[i]);
      CHECK(strcmp(ps5px_basename(mine), basename(theirs)) == 0);
   }
   CHECK(strcmp(ps5px_basename(NULL), ".") == 0);
   char empty[1] = "";
   CHECK(strcmp(ps5px_basename(empty), ".") == 0);

   const char text[] = "https://example.org/a/b?c=/d";
   CHECK(ps5px_memrchr(text, '/', sizeof(text) - 1) == memrchr(text, '/', sizeof(text) - 1));
   CHECK(ps5px_memrchr(text, 'h', sizeof(text) - 1) == text);
   CHECK(ps5px_memrchr(text, 'Z', sizeof(text) - 1) == NULL);
   CHECK(ps5px_memrchr(text, '/', 0) == NULL);
}

static void
test_rest(void)
{
   int fds[2] = {-1, -1};
   CHECK(ps5px_pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
   CHECK((fcntl(fds[0], F_GETFL) & O_NONBLOCK) && (fcntl(fds[1], F_GETFL) & O_NONBLOCK));
   CHECK((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) && (fcntl(fds[1], F_GETFD) & FD_CLOEXEC));
   char byte = 0;
   CHECK(read(fds[0], &byte, 1) == -1 && errno == EAGAIN);
   CHECK(write(fds[1], "x", 1) == 1 && read(fds[0], &byte, 1) == 1 && byte == 'x');
   close(fds[0]);
   close(fds[1]);

   CHECK(ps5px_getprotobyname("tcp") && ps5px_getprotobyname("tcp")->p_proto == IPPROTO_TCP);
   CHECK(ps5px_getprotobyname("udp") && ps5px_getprotobyname("udp")->p_proto == IPPROTO_UDP);
   CHECK(ps5px_getprotobyname("sctp") == NULL && ps5px_getprotobyname(NULL) == NULL);

   char name[32];
   CHECK(ps5px_getlogin_r(name, sizeof(name)) == 0 && strcmp(name, "guest") == 0);
   CHECK(ps5px_getlogin_r(name, 3) == ERANGE);
   CHECK(ps5px_gethostname(name, sizeof(name)) == 0 && strcmp(name, "PS5") == 0);
   for (int i = 0; i < 1000; i++)
      CHECK(ps5px_random() >= 0);
   CHECK(ps5px_pathconf("/", _PC_NAME_MAX) == 255 && ps5px_pathconf("/", _PC_PATH_MAX) == 1024);
   errno = 0;
   CHECK(ps5px_pathconf("/", 9999) == -1 && errno == EINVAL);
}

/* recvmsg: one datagram into one buffer (what the DEV9 sockets and curl ask for). */
static void
test_recvmsg(void)
{
   int pair[2];
   CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);

   char buffer[32] = {0};
   struct iovec vector = {.iov_base = buffer, .iov_len = sizeof(buffer)};
   struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_flags = 0x55};
   message.msg_controllen = 12;

   CHECK(send(pair[0], "datagram", 8, 0) == 8);
   CHECK(ps5px_recvmsg(pair[1], &message, 0) == 8);
   CHECK(memcmp(buffer, "datagram", 8) == 0);
   CHECK(message.msg_controllen == 0);
   CHECK(message.msg_flags == 0);

   /* nothing waiting: the socket's own error comes through */
   errno = 0;
   CHECK(ps5px_recvmsg(pair[1], &message, MSG_DONTWAIT) == -1);
   CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

   /* scattered reads are not what the stand-in does: it says so instead of reading part */
   struct iovec two[2] = {vector, vector};
   struct msghdr scattered = {.msg_iov = two, .msg_iovlen = 2};
   CHECK(send(pair[0], "x", 1, 0) == 1);
   errno = 0;
   CHECK(ps5px_recvmsg(pair[1], &scattered, 0) == -1);
   CHECK(errno == EINVAL);
   errno = 0;
   CHECK(ps5px_recvmsg(pair[1], NULL, 0) == -1);
   CHECK(errno == EINVAL);
   /* and the datagram is still there */
   CHECK(ps5px_recvmsg(pair[1], &message, 0) == 1);
   CHECK(buffer[0] == 'x');

   /* a sender's address is reported with its length */
   int udp_in = socket(AF_INET, SOCK_DGRAM, 0), udp_out = socket(AF_INET, SOCK_DGRAM, 0);
   struct sockaddr_in local = {.sin_family = AF_INET};
   local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   socklen_t local_length = sizeof(local);
   if (udp_in >= 0 && udp_out >= 0 && bind(udp_in, (struct sockaddr *)&local, sizeof(local)) == 0 &&
       getsockname(udp_in, (struct sockaddr *)&local, &local_length) == 0) {
      struct sockaddr_in from;
      memset(&from, 0, sizeof(from));
      struct msghdr named = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &vector, .msg_iovlen = 1};
      CHECK(sendto(udp_out, "udp", 3, 0, (struct sockaddr *)&local, sizeof(local)) == 3);
      CHECK(ps5px_recvmsg(udp_in, &named, 0) == 3);
      CHECK(named.msg_namelen == sizeof(struct sockaddr_in));
      CHECK(from.sin_family == AF_INET && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
   }
   if (udp_in >= 0) close(udp_in);
   if (udp_out >= 0) close(udp_out);
   close(pair[0]);
   close(pair[1]);
}

int
main(void)
{
   test_getaddrinfo();
   test_timegm();
   test_strings();
   test_rest();
   test_recvmsg();
   fprintf(stderr, "libc stand-ins: %d checks, %d failed\n", checks, failures);
   return failures ? 1 : 0;
}
