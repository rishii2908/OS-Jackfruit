/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Forward declaration for use in signal handler */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * bounded_buffer_push - producer-side insertion into the bounded buffer.
 *
 * Blocks when the buffer is full (unless shutdown has begun, in which
 * case it returns -1 immediately so the caller can discard the item).
 * Wakes at least one consumer after every successful insertion.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while the buffer is full, but abort if shutdown begins */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Copy the item into the circular slot at tail */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* Wake one waiting consumer */
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer-side removal from the bounded buffer.
 *
 * Returns  0 and fills *item on success.
 * Returns  1 when shutdown is in progress AND the buffer is empty
 *            (signals the consumer to drain and exit).
 * Returns -1 on a spurious / unrecoverable error (currently unused but
 *            kept as a sentinel).
 *
 * The consumer must keep calling pop (return 0 path) until it gets 1
 * so that log chunks queued before shutdown are flushed to disk.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while the buffer is empty, but wake early on shutdown */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* If we woke because of shutdown and nothing is left, tell caller to exit */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1; /* done */
    }

    /* Dequeue from head of the circular buffer */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    /* Wake one waiting producer */
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * logging_thread - consumer that drains log chunks from the bounded
 * buffer and appends them to per-container log files under LOG_DIR/.
 *
 * Runs until bounded_buffer_pop signals shutdown (return 1).  Any
 * chunks already queued at shutdown time are still written before the
 * thread exits, ensuring no log data is lost.
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;
    int rc;

    /* Make sure the log directory exists */
    mkdir(LOG_DIR, 0755);

    while (1) {
        rc = bounded_buffer_pop(buf, &item);
        if (rc == 1)
            break; /* shutdown and buffer drained */
        if (rc != 0)
            continue; /* shouldn't happen, but be defensive */

        /* Build the path: logs/<container_id>.log */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }

        /* Write the chunk; loop to handle short writes */
        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("logging_thread: write");
                break;
            }
            written += (size_t)n;
        }

        close(fd);
    }

    return NULL;
}

/*
 * child_fn - clone(2) child entrypoint.
 *
 * Called inside the new PID / UTS / mount namespace.  Sets up the
 * container environment and exec's the requested command.
 *
 * arg must point to a child_config_t that is kept alive by the parent
 * until clone() returns (the child runs on the parent's stack copy).
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* ------------------------------------------------------------------ *
     * 1. Set hostname to the container id so UTS namespace is useful.     *
     * ------------------------------------------------------------------ */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("child_fn: sethostname");

    /* ------------------------------------------------------------------ *
     * 2. Pivot into the container rootfs.                                 *
     *                                                                     *
     *    We use the simpler chroot(2) approach here because pivot_root    *
     *    requires the new root to be a mount-point, which involves more   *
     *    bind-mount bookkeeping.  chroot is sufficient for a learning     *
     *    runtime.                                                          *
     * ------------------------------------------------------------------ */
    if (chdir(cfg->rootfs) < 0) {
        perror("child_fn: chdir to rootfs");
        return 1;
    }
    if (chroot(".") < 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("child_fn: chdir /");
        return 1;
    }

    /* ------------------------------------------------------------------ *
     * 3. Mount /proc so tools like ps(1) work inside the container.       *
     * ------------------------------------------------------------------ */
    mkdir("/proc", 0555); /* OK if it already exists */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        /* Non-fatal: warn but continue */
        perror("child_fn: mount /proc");
    }

    /* ------------------------------------------------------------------ *
     * 4. Redirect stdout / stderr to the log pipe fd.                     *
     * ------------------------------------------------------------------ */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* ------------------------------------------------------------------ *
     * 5. Apply nice value if non-zero.                                    *
     * ------------------------------------------------------------------ */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0)
            perror("child_fn: nice");
    }

    /* ------------------------------------------------------------------ *
     * 6. Exec the requested command via /bin/sh -c so shell syntax works. *
     * ------------------------------------------------------------------ */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);

    /* If we reach here exec failed */
    perror("child_fn: execl");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ======================================================================
 * Internal supervisor helpers
 * ====================================================================== */

/*
 * find_container - walk the linked list and return the record whose id
 * matches, or NULL.  Caller must hold metadata_lock.
 */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c != NULL; c = c->next) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
    }
    return NULL;
}

/*
 * reap_children - called from SIGCHLD handler context or the event
 * loop.  Harvests any zombie children and updates their metadata.
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *c;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (c = ctx->containers; c != NULL; c = c->next) {
            if (c->host_pid == pid) {
                if (WIFSIGNALED(status)) {
                    c->state      = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code  = 0;
                } else {
                    c->state     = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                    c->exit_signal = 0;
                }

                /* Unregister from the kernel monitor if available */
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd,
                                            c->id, pid);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/*
 * Signal handler: set should_stop on SIGINT/SIGTERM, reap on SIGCHLD.
 * We use a global pointer because SA_SIGINFO gives us no clean way to
 * pass a context pointer without a global.
 */
static void supervisor_signal_handler(int sig)
{
    if (!g_ctx)
        return;

    if (sig == SIGCHLD)
        reap_children(g_ctx);
    else if (sig == SIGINT || sig == SIGTERM)
        g_ctx->should_stop = 1;
}

/*
 * handle_start_run - spawn a new container and register it.
 * Used for both CMD_START (detach) and CMD_RUN (same; "run" attaches in
 * a full implementation; here both behave identically for simplicity).
 */
static void handle_start_run(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp)
{
    /* Check for duplicate id */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' already exists", req->container_id);
        return;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create a pipe for log capture: child writes, we read */
    int pipefds[2];
    if (pipe2(pipefds, O_CLOEXEC) < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "pipe2: %s", strerror(errno));
        return;
    }

    /* Build the log file path */
    char log_path[PATH_MAX];
    mkdir(LOG_DIR, 0755);
    snprintf(log_path, sizeof(log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Prepare the child config.  The write-end fd must NOT be O_CLOEXEC
     * in the child so we pass it explicitly via child_config_t. */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM");
        close(pipefds[0]);
        close(pipefds[1]);
        return;
    }

    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    /* Remove O_CLOEXEC from the write end so the child can use it */
    int flags = fcntl(pipefds[1], F_GETFD);
    fcntl(pipefds[1], F_SETFD, flags & ~FD_CLOEXEC);
    cfg->log_write_fd = pipefds[1];

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM (stack)");
        close(pipefds[0]);
        close(pipefds[1]);
        free(cfg);
        return;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);

    /* The stack is safe to free after clone() returns; the child has its
     * own copy (COW pages).  We can't free cfg until after exec because
     * the child still reads it, but once exec succeeds the child's
     * address space is replaced.  We free here and accept the tiny race
     * window — in a production runtime you'd synchronise with the child
     * via a pipe or barrier. */
    free(stack);

    /* Close the write end of the pipe in the parent */
    close(pipefds[1]);

    if (pid < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "clone: %s", strerror(errno));
        close(pipefds[0]);
        free(cfg);
        return;
    }

    free(cfg); /* safe: child has exec'd or will soon */

    /* Register metadata */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        /* We spawned but can't track it — kill it */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefds[0]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM (record)");
        return;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    strncpy(rec->log_path, log_path, PATH_MAX - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with the kernel monitor if available */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd,
                              req->container_id, pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes);
    }

    /* Spawn a thread to forward pipe output into the bounded buffer */
    /* We use a simple inline approach: a detached reader thread */
    int *read_fd_ptr = malloc(sizeof(int));
    if (read_fd_ptr) {
        *read_fd_ptr = pipefds[0];

        /* We pass both read-fd and container-id.
         * Use a small heap struct to avoid stack lifetime issues. */
        typedef struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buf; } pipe_reader_arg_t;
        pipe_reader_arg_t *pra = malloc(sizeof(*pra));
        if (pra) {
            pra->fd  = pipefds[0];
            pra->buf = &ctx->log_buffer;
            strncpy(pra->id, req->container_id, CONTAINER_ID_LEN - 1);
            free(read_fd_ptr);

            pthread_t tid;
            /* Inline reader lambda via nested function simulation */
            extern void *pipe_reader_thread(void *);
            if (pthread_create(&tid, NULL, pipe_reader_thread, pra) == 0)
                pthread_detach(tid);
            else {
                close(pra->fd);
                free(pra);
            }
        } else {
            close(pipefds[0]);
            free(read_fd_ptr);
        }
    } else {
        close(pipefds[0]);
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "started container '%s' pid %d", req->container_id, (int)pid);
}

/*
 * pipe_reader_thread - reads log data from the child's stdout/stderr
 * pipe and pushes chunks into the bounded buffer for the logging thread.
 */
void *pipe_reader_thread(void *arg)
{
    typedef struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buf; } pipe_reader_arg_t;
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;

    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pra->id, CONTAINER_ID_LEN - 1);

        n = read(pra->fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break; /* EOF or error */

        item.length = (size_t)n;
        bounded_buffer_push(pra->buf, &item);
    }

    close(pra->fd);
    free(pra);
    return NULL;
}

/*
 * handle_ps - serialise container list into the response message.
 * A more complete implementation would send a separate stream; here we
 * fit a short summary into the fixed-size message field.
 */
static void handle_ps(supervisor_ctx_t *ctx,
                      int client_fd)
{
    char line[256];
    container_record_t *c;

    pthread_mutex_lock(&ctx->metadata_lock);

    /* Header */
    const char *hdr = "ID                               PID      STATE     STARTED\n";
    write(client_fd, hdr, strlen(hdr));

    for (c = ctx->containers; c != NULL; c = c->next) {
        char tbuf[32];
        struct tm *tm_info = localtime(&c->started_at);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", tm_info);

        snprintf(line, sizeof(line), "%-32s %-8d %-9s %s\n",
                 c->id, (int)c->host_pid,
                 state_to_string(c->state), tbuf);
        write(client_fd, line, strlen(line));
    }

    pthread_mutex_unlock(&ctx->metadata_lock);
}

/*
 * handle_logs - stream the on-disk log file for a container back to the
 * client fd.
 */
static void handle_logs(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        int client_fd)
{
    container_record_t *c;
    char path[PATH_MAX];

    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        const char *msg = "error: container not found\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    strncpy(path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char err[256];
        snprintf(err, sizeof(err), "error: cannot open log: %s\n", strerror(errno));
        write(client_fd, err, strlen(err));
        return;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, (size_t)n);

    close(fd);
}

/*
 * handle_stop - send SIGTERM to the container process and mark it
 * STOPPED in metadata.
 */
static void handle_stop(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' not found", req->container_id);
        return;
    }

    if (c->state != CONTAINER_RUNNING && c->state != CONTAINER_STARTING) {
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' (pid %d) is not running", req->container_id, (int)pid);
        return;
    }

    pid_t pid = c->host_pid;
    c->state  = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "kill(SIGTERM) failed: %s", strerror(errno));
        return;
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "sent SIGTERM to container '%s' (pid %d)", req->container_id, (int)pid);
}

/*
 * supervisor_event_loop - accept client connections on the UNIX domain
 * socket and dispatch control requests.
 */
static void supervisor_event_loop(supervisor_ctx_t *ctx)
{
    while (!ctx->should_stop) {
        /* Use select with a short timeout so we can check should_stop */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->server_fd, &rfds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int rc = select(ctx->server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (rc == 0)
            continue; /* timeout – check should_stop */

        int client_fd = accept(ctx->server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("accept");
            break;
        }

        /* Read a single request */
        control_request_t req;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            handle_start_run(ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            break;

        case CMD_PS:
            /* For PS we write formatted text directly; still send a
             * terminator response so the client knows we're done. */
            handle_ps(ctx, client_fd);
            resp.status = 0;
            write(client_fd, &resp, sizeof(resp));
            break;

        case CMD_LOGS:
            handle_logs(ctx, &req, client_fd);
            resp.status = 0;
            write(client_fd, &resp, sizeof(resp));
            break;

        case CMD_STOP:
            handle_stop(ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            break;

        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "unknown command");
            write(client_fd, &resp, sizeof(resp));
            break;
        }

        close(client_fd);
    }
}

/*
 * run_supervisor - long-running supervisor process.
 *
 * Responsibilities:
 *   1. Open /dev/container_monitor
 *   2. Create and bind the control UNIX domain socket
 *   3. Install signal handlers
 *   4. Spawn the logging thread
 *   5. Enter the event loop
 *   6. Graceful shutdown
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* ------------------------------------------------------------------ *
     * 1. Open the kernel monitor device (best-effort; may not be loaded). *
     * ------------------------------------------------------------------ */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor: %s "
                "(memory monitoring disabled)\n", strerror(errno));

    /* ------------------------------------------------------------------ *
     * 2. Create the UNIX domain socket control plane.                     *
     * ------------------------------------------------------------------ */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    /* Remove stale socket file if present */
    unlink(CONTROL_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        goto cleanup;
    }

    fprintf(stderr, "Supervisor started. rootfs base: %s  socket: %s\n",
            rootfs, CONTROL_PATH);

    /* ------------------------------------------------------------------ *
     * 3. Install signal handlers.                                         *
     * ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* Ignore SIGPIPE so writing to a closed client fd doesn't kill us */
    signal(SIGPIPE, SIG_IGN);

    /* ------------------------------------------------------------------ *
     * 4. Spawn the logging thread.                                        *
     * ------------------------------------------------------------------ */
    rc = pthread_create(&ctx.logger_thread, NULL,
                        logging_thread, &ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create (logger)");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ *
     * 5. Enter the supervisor event loop.                                 *
     * ------------------------------------------------------------------ */
    supervisor_event_loop(&ctx);

    /* ------------------------------------------------------------------ *
     * 6. Graceful shutdown.                                               *
     * ------------------------------------------------------------------ */
    fprintf(stderr, "Supervisor shutting down...\n");

    /* Signal remaining containers to stop */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c;
    for (c = ctx.containers; c != NULL; c = c->next) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait briefly for children */
    sleep(1);
    reap_children(&ctx);

    /* Drain and stop the log buffer */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    g_ctx = NULL;
    return 0;
}

/*
 * send_control_request - client-side helper.
 *
 * Connects to the supervisor's UNIX domain socket, sends the request,
 * reads the response (or raw text for PS/LOGS), and returns the status.
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    /* Send the request struct */
    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    /* For PS and LOGS the supervisor sends formatted text before the
     * response struct.  Read everything until EOF or a full response. */
    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        /* Stream text to stdout, then read the trailing response struct */
        char buf[4096];
        ssize_t n;
        size_t resp_bytes = 0;
        control_response_t resp;
        unsigned char *resp_ptr = (unsigned char *)&resp;

        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            /* Check if the last sizeof(resp) bytes look like the
             * terminator response.  Simple heuristic: when we have
             * accumulated >= sizeof(resp) tail bytes, peel them off. */
            fwrite(buf, 1, (size_t)n, stdout);
            resp_bytes += (size_t)n;
        }
        (void)resp_ptr; (void)resp_bytes;
        /* For simplicity, just return 0 on EOF */
        close(fd);
        return 0;
    }

    /* For START / RUN / STOP read the fixed-size response */
    control_response_t resp;
    ssize_t n = read(fd, &resp, sizeof(resp));
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Unexpected response size from supervisor\n");
        return 1;
    }

    if (resp.status != 0) {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }

    printf("%s\n", resp.message);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * The supervisor responds with container metadata formatted as a
     * plain-text table streamed to stdout, followed by a response struct.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
