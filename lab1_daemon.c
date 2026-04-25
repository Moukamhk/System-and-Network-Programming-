/*
 * lab1_daemon.c
 * CEF488 – Systems and Network Programming  |  Lab 1
 *
 * Implements:
 *   Task 1 – daemonization, bidirectional unnamed pipes, non-blocking I/O
 *   Task 2 – signal handling (SIGTERM, SIGINT, SIGCHLD) via sigaction()
 *   Task 3 – setrlimit() resource exhaustion, flock() PID-file locking,
 *             child-response timeout
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -o lab1_daemon lab1_daemon.c          (daemon)
 *   gcc -Wall -Wextra -Werror -DDEBUG -o lab1_debug lab1_daemon.c   (debug)
 */

/* Feature macros come from the Makefile (-D flags) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/file.h>   /* flock() */
#include <syslog.h>
#include <time.h>

#include "lab1_common.h"

/* ═══════════════════════════════════════════════════════════════════
 * Global signal flags
 * ══════════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t g_running  = 1;  /* main loop sentinel  */
static volatile sig_atomic_t g_child_done = 0; /* set by SIGCHLD handler */

/* ═══════════════════════════════════════════════════════════════════
 * Signal handlers  (async-signal-safe: only write(), _exit(), waitpid())
 * ══════════════════════════════════════════════════════════════════ */
static void handle_term(int sig)
{
    (void)sig;
    g_running = 0;  /* tell main loop to stop */
}

static void handle_chld(int sig)
{
    (void)sig;
    /* reap ALL terminated children – loop to avoid missed signals */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    g_child_done = 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * install_handler – thin wrapper around sigaction()
 * ══════════════════════════════════════════════════════════════════ */
static void install_handler(int signo, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;

    if (sigaction(signo, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction(%d): %s", signo, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * daemonize – classic 6-step daemonisation
 * ══════════════════════════════════════════════════════════════════ */
static void daemonize(void) __attribute__((unused));
static void daemonize(void)
{
    pid_t pid;
    int   fd, maxfd;

    /* Step 1: fork so the child is never a process-group leader */
    pid = fork();
    if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS);   /* parent exits */

    /* Step 2: new session → detach from controlling terminal */
    if (setsid() == -1) { perror("setsid"); exit(EXIT_FAILURE); }

    /* Step 3: change root so we don't hold a mounted FS busy */
    if (chdir("/") == -1) { perror("chdir"); exit(EXIT_FAILURE); }

    /* Step 4: predictable file permissions */
    umask(0);

    /* Step 5: close every inherited file descriptor */
    maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;   /* conservative fallback */
    for (fd = 0; fd < maxfd; fd++)
        close(fd);

    /* Step 6: redirect stdin/stdout/stderr to /dev/null */
    fd = open("/dev/null", O_RDWR);  /* fd 0 = stdin  */
    if (fd != 0) { syslog(LOG_ERR, "open /dev/null failed"); exit(EXIT_FAILURE); }
    dup2(fd, STDOUT_FILENO);         /* fd 1 = stdout */
    dup2(fd, STDERR_FILENO);         /* fd 2 = stderr */
}

/* ═══════════════════════════════════════════════════════════════════
 * write_pid_file  – create & exclusively lock the PID file
 * returns the open fd (kept locked for the process lifetime)
 * ══════════════════════════════════════════════════════════════════ */
static int write_pid_file(void)
{
    int  fd;
    char buf[32];

    fd = open(PID_FILE, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open PID file: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Try to get an exclusive lock – fails immediately if another instance holds it */
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            syslog(LOG_ERR, "Another daemon instance is already running. Exiting.");
            fprintf(stderr, "Another daemon instance is already running.\n");
        } else {
            syslog(LOG_ERR, "flock: %s", strerror(errno));
        }
        exit(EXIT_FAILURE);
    }

    /* Truncate then write the PID */
    ftruncate(fd, 0);
    snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, strlen(buf)) == -1) {
        syslog(LOG_ERR, "write PID: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    DLOG("PID file written: %s (pid=%d)", PID_FILE, (int)getpid());
    return fd;  /* keep open to hold the lock */
}

/* ═══════════════════════════════════════════════════════════════════
 * set_nonblock – apply O_NONBLOCK to a file descriptor
 * ══════════════════════════════════════════════════════════════════ */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        syslog(LOG_ERR, "fcntl O_NONBLOCK: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * nanosleep_ms – sleep for ms milliseconds
 * ══════════════════════════════════════════════════════════════════ */
static void nanosleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * child_process – runs the accumulator child
 *   reads ints from pipe_in, writes running sum back on pipe_out
 * ══════════════════════════════════════════════════════════════════ */
static void child_process(int pipe_in, int pipe_out)
{
    int running_sum = 0;

    DLOG("child started, pid=%d", (int)getpid());

    set_nonblock(pipe_in);   /* non-blocking reads so child can do other work */

    while (1) {
        int val;
        ssize_t n = read(pipe_in, &val, sizeof(val));

        if (n == sizeof(val)) {
            running_sum += val;
            DLOG("child: got %d, sum=%d", val, running_sum);

            /* blocking write back – we must not drop the sum */
            if (write(pipe_out, &running_sum, sizeof(running_sum)) == -1) {
                syslog(LOG_ERR, "child write: %s", strerror(errno));
                break;
            }
        } else if (n == 0) {
            /* parent closed write end → EOF, time to exit */
            DLOG("child: EOF, exiting");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* no data yet – simulate other useful work */
                nanosleep_ms(100);
            } else {
                syslog(LOG_ERR, "child read: %s", strerror(errno));
                break;
            }
        }
    }

    close(pipe_in);
    close(pipe_out);
    DLOG("child exiting cleanly");
    exit(EXIT_SUCCESS);
}

/* ═══════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    /* ── syslog before daemonization so we can log early errors ── */
    openlog("lab1_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "daemon starting");

#ifndef DEBUG
    daemonize();
#else
    DLOG("running in DEBUG mode – skipping daemonization");
#endif

    /* ── PID file (also acts as instance lock) ── */
    int pid_fd = write_pid_file();

    /* ── Install signal handlers ── */
    install_handler(SIGTERM, handle_term, SA_RESTART);
    install_handler(SIGINT,  handle_term, SA_RESTART);
    /* SA_RESTART: interrupted system calls restart automatically */
    install_handler(SIGCHLD, handle_chld, SA_RESTART | SA_NOCLDSTOP);

    /* ── Create two unnamed pipes for bidirectional IPC ──
     *   pipe_p2c: parent writes, child reads
     *   pipe_c2p: child writes, parent reads
     * ─────────────────────────────────────────────────── */
    int pipe_p2c[2], pipe_c2p[2];

    if (pipe(pipe_p2c) == -1 || pipe(pipe_c2p) == -1) {
        syslog(LOG_ERR, "pipe(): %s", strerror(errno));
        unlink(PID_FILE);
        exit(EXIT_FAILURE);
    }

    /* ── Task 3: simulate resource exhaustion with setrlimit() ──
     *
     *   We read RLIMIT_NPROC BEFORE daemonizing (done above).
     *   Now we apply a soft limit = current_soft + 1 so that:
     *     - The FIRST  fork() → worker child  → succeeds
     *     - The SECOND fork() → demo attempt  → fails EAGAIN
     *
     *   We use the kernel's own accounting (rlim_cur) rather than
     *   counting /proc entries, which is racy inside a new session.
     * ──────────────────────────────────────────────────────────── */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NPROC, &rl) == -1) {
        syslog(LOG_WARNING, "getrlimit: %s (non-fatal)", strerror(errno));
        rl.rlim_cur = 1024; /* safe fallback */
        rl.rlim_max = 1024;
    }

    /* current soft limit already reflects how many procs our UID has.
     * Adding 1 gives room for exactly one more fork(). */
    rlim_t new_limit = rl.rlim_cur + 1;
    if (new_limit > rl.rlim_max) new_limit = rl.rlim_max;
    rl.rlim_cur = new_limit;

    if (setrlimit(RLIMIT_NPROC, &rl) == -1) {
        syslog(LOG_WARNING, "setrlimit: %s (non-fatal)", strerror(errno));
    } else {
        syslog(LOG_INFO, "RLIMIT_NPROC soft limit → %lu",
               (unsigned long)rl.rlim_cur);
    }
    DLOG("RLIMIT_NPROC set to %lu", (unsigned long)rl.rlim_cur);

    /* ── Fork the worker child ── */
    pid_t child_pid = fork();
    if (child_pid == -1) {
        syslog(LOG_ERR, "fork child: %s", strerror(errno));
        unlink(PID_FILE);
        exit(EXIT_FAILURE);
    }

    if (child_pid == 0) {
        /* ╔══ CHILD ══╗ */
        close(pipe_p2c[1]);  /* child does not write to p2c */
        close(pipe_c2p[0]);  /* child does not read from c2p */
        close(pid_fd);       /* child does not own PID lock  */
        child_process(pipe_p2c[0], pipe_c2p[1]);
        /* never returns */
    }

    /* ╔══ PARENT (daemon) ══╗ */
    close(pipe_p2c[0]);  /* parent does not read from p2c */
    close(pipe_c2p[1]);  /* parent does not write to c2p  */

    /* non-blocking on write (parent) and read (parent) ends */
    set_nonblock(pipe_p2c[1]);
    set_nonblock(pipe_c2p[0]);

    syslog(LOG_INFO, "daemon running, child pid=%d", (int)child_pid);

    /* ── Task 3: attempt a SECOND fork to demonstrate EAGAIN ── */
    pid_t second_child = fork();
    if (second_child == -1) {
        /* expected to fail due to setrlimit */
        syslog(LOG_WARNING, "Second fork() failed (resource limit): %s – continuing",
               strerror(errno));
        DLOG("Expected: second fork failed with EAGAIN/ENOMEM");
    } else if (second_child == 0) {
        /* If it somehow succeeded, just exit */
        exit(EXIT_SUCCESS);
    }

    /* ── Main event loop ── */
    int        sent  = 0;       /* how many ints we've sent */
    time_t     last_reply = time(NULL);

    while (g_running && sent <= ITERATIONS) {

        /* ── Send next integer to child ── */
        if (sent < ITERATIONS) {
            int val = sent + 1;   /* send 1, 2, 3, … ITERATIONS */
            ssize_t w = write(pipe_p2c[1], &val, sizeof(val));
            if (w == sizeof(val)) {
                DLOG("parent: sent %d", val);
                sent++;
            } else if (w == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* pipe buffer full – back off */
                DLOG("parent: pipe full, backing off");
            } else if (w == -1) {
                syslog(LOG_ERR, "parent write: %s", strerror(errno));
                break;
            }
        } else {
            /* All integers sent – close write end to signal EOF to child */
            close(pipe_p2c[1]);
            pipe_p2c[1] = -1;   /* sentinel so we don't close twice */
            sent++;              /* sent > ITERATIONS → skip this block */
        }

        /* ── Simulate the parent doing other work ── */
        nanosleep_ms(1000);

        /* ── Try to read the accumulated sum from child ── */
        int reply;
        ssize_t r = read(pipe_c2p[0], &reply, sizeof(reply));
        if (r == sizeof(reply)) {
            syslog(LOG_INFO, "running sum from child: %d", reply);
            DLOG("parent: got sum=%d", reply);
            last_reply = time(NULL);
        } else if (r == 0) {
            /* child closed pipe – it's done */
            DLOG("parent: child pipe EOF");
            break;
        } else if (r == -1 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
            syslog(LOG_ERR, "parent read: %s", strerror(errno));
            break;
        }

        /* ── Task 3: child timeout ── */
        if (time(NULL) - last_reply > CHILD_TIMEOUT) {
            syslog(LOG_WARNING, "child timed out – terminating");
            kill(child_pid, SIGTERM);
            g_running = 0;
            break;
        }
    }

    /* ── Drain any remaining replies ── */
    if (pipe_p2c[1] != -1) close(pipe_p2c[1]);

    {
        int reply;
        while (read(pipe_c2p[0], &reply, sizeof(reply)) == sizeof(reply))
            syslog(LOG_INFO, "final sum from child: %d", reply);
    }

    /* ── Graceful shutdown ── */
    syslog(LOG_INFO, "shutting down");

    kill(child_pid, SIGTERM);
    waitpid(child_pid, NULL, 0);

    close(pipe_c2p[0]);
    close(pid_fd);
    unlink(PID_FILE);

    syslog(LOG_INFO, "daemon exited cleanly");
    closelog();

    return EXIT_SUCCESS;
}
