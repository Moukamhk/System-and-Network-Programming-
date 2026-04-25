#ifndef LAB1_COMMON_H
#define LAB1_COMMON_H

/* ── tunables ── */
#define PID_FILE        "/tmp/mydaemon.pid"
#define LOG_IDENT       "lab1_daemon"
#define ITERATIONS      10          /* integers sent parent→child */
#define CHILD_TIMEOUT   5           /* seconds before parent gives up on child */
#define PIPE_BUF_SZ     sizeof(int) /* we only pass plain ints */

/* ── ANSI helpers (debug mode only) ── */
#ifdef DEBUG
#  define DLOG(fmt, ...) fprintf(stderr, "[DBG %s:%d] " fmt "\n", \
                                  __func__, __LINE__, ##__VA_ARGS__)
#else
#  define DLOG(fmt, ...) /* nothing in daemon mode */
#endif

#endif /* LAB1_COMMON_H */
