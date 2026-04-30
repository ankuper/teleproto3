/*
    Platform compatibility shims.

    Abstracts OS differences so the rest of the codebase can stay
    Linux-idiomatic while still compiling on macOS (development builds).
*/

#pragma once

#ifdef __APPLE__

/* TCP keepalive: macOS uses TCP_KEEPALIVE where Linux uses TCP_KEEPIDLE */
#include <netinet/tcp.h>
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

/* SOL_IP doesn't exist on macOS */
#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

/* macOS doesn't support SOCK_NONBLOCK/SOCK_CLOEXEC as socket() type flags.
   Define them to 0 so socket() calls compile, then apply via fcntl. */
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

/* TCP_WINDOW_CLAMP and TCP_QUICKACK are Linux-only */
#ifndef TCP_WINDOW_CLAMP
#define TCP_WINDOW_CLAMP 0
#endif
#ifndef TCP_QUICKACK
#define TCP_QUICKACK 0
#endif

/* SIGPOLL doesn't exist on macOS; it's the same as SIGIO on Linux */
#ifndef SIGPOLL
#define SIGPOLL SIGIO
#endif

/* macOS has no real-time signals.  SIGRTMIN/SIGRTMAX don't exist.
   Map to signal numbers above the standard ones (macOS allows up to 31).
   SIGUSR2 (31) is used as the job-interrupt signal. */
#ifndef SIGRTMAX
#define SIGRTMAX 31
#endif
#ifndef SIGRTMIN
#define SIGRTMIN 17
#endif

#include <fcntl.h>

static inline void platform_socket_post_create (int fd) {
  if (fd >= 0) {
    fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
    fcntl (fd, F_SETFD, fcntl (fd, F_GETFD) | FD_CLOEXEC);
  }
}

/* Thread ID: macOS has no gettid(), use pthread_threadid_np */
#include <pthread.h>

static inline long platform_gettid (void) {
  uint64_t tid;
  pthread_threadid_np (NULL, &tid);
  return (long) tid;
}

#else /* Linux */

static inline void platform_socket_post_create (int fd) { (void) fd; }

#include <sys/syscall.h>
#include <unistd.h>

static inline long platform_gettid (void) {
  return syscall (SYS_gettid);
}

#endif
