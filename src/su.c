/*
** Copyright 2021, Corellium, LLC
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdint.h>
#include <pwd.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sys/types.h>
#include <netinet/in.h> 

#include "su.h"
#include "utils.h"

extern int is_daemon;
extern int daemon_from_uid;
extern int daemon_from_pid;

static void populate_environment(const struct su_context *ctx) {
    struct passwd *pw;

    if (ctx->to.keepenv)
        return;

    pw = getpwuid(ctx->to.uid);
    if (pw) {
        setenv("HOME", pw->pw_dir, 1);
        if (ctx->to.shell)
            setenv("SHELL", ctx->to.shell, 1);
        else
            setenv("SHELL", DEFAULT_SHELL, 1);
        if (ctx->to.login || ctx->to.uid) {
            setenv("USER", pw->pw_name, 1);
            setenv("LOGNAME", pw->pw_name, 1);
        }
    }
}

static void socket_cleanup(struct su_context *ctx) {
    if (ctx && ctx->sock_path[0]) {
        if (unlink(ctx->sock_path))
            PLOGE("unlink (%s)", ctx->sock_path);
        ctx->sock_path[0] = 0;
    }
}

/*
 * For use in signal handlers/atexit-function
 * NOTE: su_ctx points to main's local variable.
 *       It's OK due to the program uses exit(3), not return from main()
 */
static struct su_context *su_ctx = NULL;

static void cleanup(void) {
    socket_cleanup(su_ctx);
}

static void cleanup_signal(int sig) {
    socket_cleanup(su_ctx);
    exit(128 + sig);
}

static int socket_create_temp() {
    int fd;
    struct sockaddr_in sun;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        PLOGE("socket");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        goto err;
    }

    sun.sin_family = AF_INET;
    sun.sin_addr.s_addr = INADDR_ANY;
    sun.sin_port = htons(PORT);

    if (bind(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        PLOGE("bind");
        goto err;
    }

    if (listen(fd, 1) < 0) {
        PLOGE("listen");
        goto err;
    }

    return fd;
err:
    close(fd);
    return -1;
}

static int socket_accept(int serv_fd) {
    struct timeval tv;
    fd_set fds;
    int fd, rc;

    /* Wait 20 seconds for a connection, then give up. */
    tv.tv_sec = 20;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(serv_fd, &fds);
    do {
        rc = select(serv_fd + 1, &fds, NULL, NULL, &tv);
    } while (rc < 0 && errno == EINTR);
    if (rc < 1) {
        PLOGE("select");
        return -1;
    }

    fd = accept(serv_fd, NULL, NULL);
    if (fd < 0) {
        PLOGE("accept");
        return -1;
    }

    return fd;
}

static int socket_send_request(int fd, const struct su_context *ctx) {
#define write_data(fd, data, data_len)              \
do {                                                \
    size_t __len = htonl(data_len);                 \
    __len = write((fd), &__len, sizeof(__len));     \
    if (__len != sizeof(__len)) {                   \
        PLOGE("write(" #data ")");                  \
        return -1;                                  \
    }                                               \
    __len = write((fd), data, data_len);            \
    if (__len != data_len) {                        \
        PLOGE("write(" #data ")");                  \
        return -1;                                  \
    }                                               \
} while (0)

#define write_string_data(fd, name, data)        \
do {                                        \
    write_data(fd, name, strlen(name));     \
    write_data(fd, data, strlen(data));     \
} while (0)

// stringify everything.
#define write_token(fd, name, data)         \
do {                                        \
    char buf[16];                           \
    snprintf(buf, sizeof(buf), "%d", data); \
    write_string_data(fd, name, buf);            \
} while (0)

    write_token(fd, "version", PROTO_VERSION);
    write_token(fd, "binary.version", VERSION_CODE);
    write_token(fd, "pid", ctx->from.pid);
    write_string_data(fd, "from.name", ctx->from.name);
    write_string_data(fd, "to.name", ctx->to.name);
    write_token(fd, "from.uid", ctx->from.uid);
    write_token(fd, "to.uid", ctx->to.uid);
    write_string_data(fd, "from.bin", ctx->from.bin);
    // TODO: Fix issue where not using -c does not result a in a command
    write_string_data(fd, "command", get_command(&ctx->to));
    write_token(fd, "eof", PROTO_VERSION);
    return 0;
}

static int socket_receive_result(int fd, char *result, ssize_t result_len) {
    ssize_t len;

    LOGD("waiting for user");
    len = read(fd, result, result_len-1);
    if (len < 0) {
        PLOGE("read(result)");
        return -1;
    }
    result[len] = '\0';

    return 0;
}

static void usage(int status) {
    FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(stream,
    "Usage: su [options] [--] [-] [LOGIN] [--] [args...]\n\n"
    "Options:\n"
    "  --daemon                      start the su daemon agent\n"
    "  -c, --command COMMAND         pass COMMAND to the invoked shell\n"
    "  -h, --help                    display this help message and exit\n"
    "  -, -l, --login                pretend the shell to be a login shell\n"
    "  -m, -p,\n"
    "  --preserve-environment        do not change environment variables\n"
    "  -s, --shell SHELL             use SHELL instead of the default " DEFAULT_SHELL "\n"
    "  -v, --version                 display version number and exit\n"
    "  -V                            display version code and exit,\n");
    exit(status);
}

static __attribute__ ((noreturn)) void fail(struct su_context *ctx) {
    char *cmd = get_command(&ctx->to);

    LOGW("request failed (%u->%u %s)", ctx->from.uid, ctx->to.uid, cmd);
    fprintf(stderr, "%s\n", strerror(EACCES));
    exit(EXIT_FAILURE);
}

static __attribute__ ((noreturn)) void allow(struct su_context *ctx) {
    char *arg0;
    int argc, err;

    umask(ctx->umask);

    char *binary;
    argc = ctx->to.optind;
    if (ctx->to.command) {
        binary = ctx->to.shell;
        ctx->to.argv[--argc] = ctx->to.command;
        ctx->to.argv[--argc] = "-c";
    }
    else if (ctx->to.shell) {
        binary = ctx->to.shell;
    }
    else {
        if (ctx->to.argv[argc]) {
            binary = ctx->to.argv[argc++];
        }
        else {
            binary = DEFAULT_SHELL;
        }
    }

    arg0 = strrchr (binary, '/');
    arg0 = (arg0) ? arg0 + 1 : binary;
    if (ctx->to.login) {
        int s = strlen(arg0) + 2;
        char *p = malloc(s);

        if (!p)
            exit(EXIT_FAILURE);

        *p = '-';
        strcpy(p + 1, arg0);
        arg0 = p;
    }

    populate_environment(ctx);

    #define PARG(arg)                                    \
        (argc + (arg) < ctx->to.argc) ? " " : "",                    \
        (argc + (arg) < ctx->to.argc) ? ctx->to.argv[argc + (arg)] : ""

    LOGD("%u %s executing %u %s using binary %s : %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
            ctx->from.uid, ctx->from.bin,
            ctx->to.uid, get_command(&ctx->to), binary,
            arg0, PARG(0), PARG(1), PARG(2), PARG(3), PARG(4), PARG(5),
            (ctx->to.optind + 6 < ctx->to.argc) ? " ..." : "");

    ctx->to.argv[--argc] = arg0;
    execvp(binary, ctx->to.argv + argc);
    err = errno;
    PLOGE("exec");
    fprintf(stderr, "Cannot execute %s: %s\n", binary, strerror(err));
    exit(EXIT_FAILURE);
}

static void fork_for_samsung(void)
{
    // Samsung CONFIG_SEC_RESTRICT_SETUID wants the parent process to have
    // EUID 0, or else our setresuid() calls will be denied.  So make sure
    // all such syscalls are executed by a child process.
    int rv;

    switch (fork()) {
    case 0:
        return;
    case -1:
        PLOGE("fork");
        exit(1);
    default:
        if (wait(&rv) < 0) {
            exit(1);
        } else {
            exit(WEXITSTATUS(rv));
        }
    }
}

int main(int argc, char *argv[]) {
    return su_main(argc, argv, 1);
}

int su_main(int argc, char *argv[], int need_client) {
    // start up in daemon mode if prompted
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0) {
        return run_daemon();
    }

    int ppid = getppid();
    fork_for_samsung();

    // Sanitize all secure environment variables (from linker_environ.c in AOSP linker).
    /* The same list than GLibc at this point */
    static const char* const unsec_vars[] = {
        "GCONV_PATH",
        "GETCONF_DIR",
        "HOSTALIASES",
        "LD_AUDIT",
        "LD_DEBUG",
        "LD_DEBUG_OUTPUT",
        "LD_DYNAMIC_WEAK",
        "LD_LIBRARY_PATH",
        "LD_ORIGIN_PATH",
        "LD_PRELOAD",
        "LD_PROFILE",
        "LD_SHOW_AUXV",
        "LD_USE_LOAD_BIAS",
        "LOCALDOMAIN",
        "LOCPATH",
        "MALLOC_TRACE",
        "MALLOC_CHECK_",
        "NIS_PATH",
        "NLSPATH",
        "RESOLV_HOST_CONF",
        "RES_OPTIONS",
        "TMPDIR",
        "TZDIR",
        "LD_AOUT_LIBRARY_PATH",
        "LD_AOUT_PRELOAD",
        // not listed in linker, used due to system() call
        "IFS",
    };
    const char* const* cp   = unsec_vars;
    const char* const* endp = cp + sizeof(unsec_vars)/sizeof(unsec_vars[0]);
    while (cp < endp) {
        unsetenv(*cp);
        cp++;
    }

    LOGD("su invoked.");

    struct su_context ctx = {
        .from = {
            .pid = -1,
            .uid = 2000,
            .bin = "",
            .args = "",
            .name = "",
        },
        .to = {
            .uid = 2000,
            .login = 0,
            .keepenv = 0,
            .shell = NULL,
            .command = NULL,
            .argv = argv,
            .argc = argc,
            .optind = 0,
            .name = "",
        },
        .user = {
            .android_user_id = 0,
        },
    };
    struct stat st;
    int c, socket_serv_fd, fd;
    char buf[64], *result;
    struct option long_opts[] = {
        { "command",            required_argument,    NULL, 'c' },
        { "help",            no_argument,        NULL, 'h' },
        { "login",            no_argument,        NULL, 'l' },
        { "preserve-environment",    no_argument,        NULL, 'p' },
        { "shell",            required_argument,    NULL, 's' },
        { "version",            no_argument,        NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };

    while ((c = getopt_long(argc, argv, "+c:hlmps:Vv", long_opts, NULL)) != -1) {
        switch(c) {
        case 'c':
            ctx.to.shell = DEFAULT_SHELL;
            ctx.to.command = optarg;
            break;
        case 'h':
            usage(EXIT_SUCCESS);
            break;
        case 'l':
            ctx.to.login = 1;
            break;
        case 'm':
        case 'p':
            ctx.to.keepenv = 1;
            break;
        case 's':
            ctx.to.shell = optarg;
            break;
        case 'V':
            printf("%d\n", VERSION_CODE);
            exit(EXIT_SUCCESS);
        case 'v':
            exit(EXIT_SUCCESS);
        default:
            /* Bionic getopt_long doesn't terminate its error output by newline */
            fprintf(stderr, "\n");
            usage(2);
        }
    }

    if (need_client) {
        LOGD("starting daemon client %d %d", getuid(), geteuid());
        return connect_daemon(argc, argv, ppid);
    }

    if (optind < argc && !strcmp(argv[optind], "-")) {
        ctx.to.login = 1;
        optind++;
    }
    /* username or uid */
    if (optind < argc && strcmp(argv[optind], "--")) {
        struct passwd *pw;
        pw = getpwnam(argv[optind]);
        if (!pw) {
            char *endptr;

            /* It seems we shouldn't do this at all */
            errno = 0;
            ctx.to.uid = strtoul(argv[optind], &endptr, 10);
            if (errno || *endptr) {
                LOGE("Unknown id: %s\n", argv[optind]);
                fprintf(stderr, "Unknown id: %s\n", argv[optind]);
                exit(EXIT_FAILURE);
            }
        } else {
            ctx.to.uid = pw->pw_uid;
            if (pw->pw_name)
                strncpy(ctx.to.name, pw->pw_name, sizeof(ctx.to.name));
        }
        optind++;
    }
    if (optind < argc && !strcmp(argv[optind], "--")) {
        optind++;
    }
    ctx.to.optind = optind;

    su_ctx = &ctx;

    // Allow everything
    allow(&ctx);

    // odd perms on superuser data dir
    if (st.st_gid != st.st_uid) {
        LOGE("Bad uid/gid %d/%d for Superuser Requestor application",
                (int)st.st_uid, (int)st.st_gid);
        fail(&ctx);
    }

    ctx.umask = umask(027);

    if (setgroups(0, NULL)) {
        PLOGE("setgroups");
        fail(&ctx);
    }
    if (setegid(st.st_gid)) {
        PLOGE("setegid (%u)", st.st_gid);
        fail(&ctx);
    }
    if (seteuid(st.st_uid)) {
        PLOGE("seteuid (%u)", st.st_uid);
        fail(&ctx);
    }

    socket_serv_fd = socket_create_temp();
    LOGD("%s", ctx.sock_path);
    if (socket_serv_fd < 0) {
        fail(&ctx);
    }

    signal(SIGHUP, cleanup_signal);
    signal(SIGPIPE, cleanup_signal);
    signal(SIGTERM, cleanup_signal);
    signal(SIGQUIT, cleanup_signal);
    signal(SIGINT, cleanup_signal);
    signal(SIGABRT, cleanup_signal);

    atexit(cleanup);

    fd = socket_accept(socket_serv_fd);
    if (fd < 0) {
        fail(&ctx);
    }
    if (socket_send_request(fd, &ctx)) {
        fail(&ctx);
    }
    if (socket_receive_result(fd, buf, sizeof(buf))) {
        fail(&ctx);
    }

    close(fd);
    close(socket_serv_fd);
    socket_cleanup(&ctx);

    result = buf;

#define SOCKET_RESPONSE    "socket:"
    if (strncmp(result, SOCKET_RESPONSE, sizeof(SOCKET_RESPONSE) - 1))
        LOGW("SECURITY RISK: Requestor still receives credentials in intent");
    else
        result += sizeof(SOCKET_RESPONSE) - 1;

    if (!strcmp(result, "fail")) {
        fail(&ctx);
    } else if (!strcmp(result, "ALLOW")) {
        allow(&ctx);
    } else {
        LOGE("unknown response from Superuser Requestor: %s", result);
        fail(&ctx);
    }
}
