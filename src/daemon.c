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

#define _GNU_SOURCE /* for unshare() */

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
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sys/types.h>
#include <pthread.h>
#include <sched.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>

#include "su.h"
#include "utils.h"
#include "pts.h"

int is_daemon = 0;
int daemon_from_uid = 0;
int daemon_from_pid = 0;

// Constants for the atty bitfield
#define ATTY_IN     1
#define ATTY_OUT    2
#define ATTY_ERR    4

/*
 * Receive a file descriptor from a Unix socket.
 * Contributed by @mkasick
 *
 * Returns the file descriptor on success, or -1 if a file
 * descriptor was not actually included in the message
 *
 * On error the function terminates by calling exit(-1)
 */
static int recv_fd(int sockfd) {
    // Need to receive data from the message, otherwise don't care about it.
    char iovbuf;

    struct iovec iov = {
        .iov_base = &iovbuf,
        .iov_len  = 1,
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };

    if (recvmsg(sockfd, &msg, MSG_WAITALL) != 1) {
        goto error;
    }

    // Was a control message actually sent?
    switch (msg.msg_controllen) {
    case 0:
        // No, so the file descriptor was closed and won't be used.
        return -1;
    case sizeof(cmsgbuf):
        // Yes, grab the file descriptor from it.
        break;
    default:
        goto error;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    if (cmsg             == NULL                  ||
        cmsg->cmsg_len   != CMSG_LEN(sizeof(int)) ||
        cmsg->cmsg_level != SOL_SOCKET            ||
        cmsg->cmsg_type  != SCM_RIGHTS) {
error:
        LOGE("unable to read fd");
        exit(-1);
    }

    return *(int *)CMSG_DATA(cmsg);
}

/*
 * Send a file descriptor through a Unix socket.
 * Contributed by @mkasick
 *
 * On error the function terminates by calling exit(-1)
 *
 * fd may be -1, in which case the dummy data is sent,
 * but no control message with the FD is sent.
 */
static void send_fd(int sockfd, int fd) {
    // Need to send some data in the message, this will do.
    struct iovec iov = {
        .iov_base = "",
        .iov_len  = 1,
    };

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    if (fd != -1) {
        // Is the file descriptor actually open?
        if (fcntl(fd, F_GETFD) == -1) {
            if (errno != EBADF) {
                goto error;
            }
            // It's closed, don't send a control message or sendmsg will EBADF.
        } else {
            // It's open, send the file descriptor in a control message.
            msg.msg_control    = cmsgbuf;
            msg.msg_controllen = sizeof(cmsgbuf);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

            cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;

            *(int *)CMSG_DATA(cmsg) = fd;
        }
    }

    if (sendmsg(sockfd, &msg, 0) != 1) {
error:
        PLOGE("unable to send fd");
        exit(-1);
    }
}

static int read_int(int fd) {
    int val;
    int len = read(fd, &val, sizeof(int));
    if (len != sizeof(int)) {
        LOGE("unable to read int: %d", len);
        exit(-1);
    }
    return val;
}

static void write_int(int fd, int val) {
    int written = write(fd, &val, sizeof(int));
    if (written != sizeof(int)) {
        PLOGE("unable to write int");
        exit(-1);
    }
}

static char* read_string(int fd) {
    int len = read_int(fd);
    if (len > PATH_MAX || len < 0) {
        LOGE("invalid string length %d", len);
        exit(-1);
    }
    char* val = malloc(sizeof(char) * (len + 1));
    if (val == NULL) {
        LOGE("unable to malloc string");
        exit(-1);
    }
    val[len] = '\0';
    int amount = read(fd, val, len);
    if (amount != len) {
        LOGE("unable to read string");
        exit(-1);
    }
    return val;
}

static void write_string(int fd, char* val) {
    int len = strlen(val);
    write_int(fd, len);
    int written = write(fd, val, len);
    if (written != len) {
        PLOGE("unable to write string");
        exit(-1);
    }
}

static int run_daemon_child(int infd, int outfd, int errfd, int argc, char** argv) {
    if (-1 == dup2(outfd, STDOUT_FILENO)) {
        PLOGE("dup2 child outfd");
        exit(-1);
    }

    if (-1 == dup2(errfd, STDERR_FILENO)) {
        PLOGE("dup2 child errfd");
        exit(-1);
    }

    if (-1 == dup2(infd, STDIN_FILENO)) {
        PLOGE("dup2 child infd");
        exit(-1);
    }

    close(infd);
    close(outfd);
    close(errfd);

    return su_main(argc, argv, 0);
}

static int daemon_accept(int fd) {
    is_daemon = 1;
    int pid = read_int(fd);
    LOGD("remote pid: %d", pid);
    char *pts_slave = read_string(fd);
    LOGD("remote pts_slave: %s", pts_slave);
    daemon_from_uid = read_int(fd);
    LOGD("remote uid: %d", daemon_from_uid);
    daemon_from_pid = read_int(fd);
    LOGD("remote req pid: %d", daemon_from_pid);

    struct ucred credentials;
    int ucred_length = sizeof(struct ucred);
    /* fill in the user data structure */
    if(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, (unsigned int *)&ucred_length)) {
        LOGE("could obtain credentials from unix domain socket");
        exit(-1);
    }
    // if the credentials on the other side of the wire are NOT root,
    // we can't trust anything being sent.
/*
    if (credentials.uid != 0) {
        daemon_from_uid = credentials.uid;
        pid = credentials.pid;
        daemon_from_pid = credentials.pid;
    }
*/

    // The the FDs for each of the streams
    int infd  = recv_fd(fd);
    int outfd = recv_fd(fd);
    int errfd = recv_fd(fd);

    int argc = read_int(fd);
    if (argc < 0 || argc > 512) {
        LOGE("unable to allocate args: %d", argc);
        exit(-1);
    }
    LOGD("remote args: %d", argc);
    char** argv = (char**)malloc(sizeof(char*) * (argc + 1));
    argv[argc] = NULL;
    int i;
    for (i = 0; i < argc; i++) {
        argv[i] = read_string(fd);
    }

    // ack
    write_int(fd, 1);

    // Fork the child process. The fork has to happen before calling
    // setsid() and opening the pseudo-terminal so that the parent
    // is not affected
    int child = fork();
    if (child < 0) {
        // fork failed, send a return code and bail out
        PLOGE("unable to fork");
        write(fd, &child, sizeof(int));
        close(fd);
        return child;
    }

    if (child != 0) {
        // In parent, wait for the child to exit, and send the exit code
        // across the wire.
        int status, code;

        free(pts_slave);

        LOGD("waiting for child exit");
        if (waitpid(child, &status, 0) > 0) {
            code = WEXITSTATUS(status);
        }
        else {
            code = -1;
        }

        // Pass the return code back to the client
        LOGD("sending code");
        if (write(fd, &code, sizeof(int)) != sizeof(int)) {
            PLOGE("unable to write exit code");
        }

        close(fd);
        LOGD("child exited");
        return code;
    }

    // We are in the child now
    // Close the unix socket file descriptor
    close (fd);

    // Become session leader
    if (setsid() == (pid_t) -1) {
        PLOGE("setsid");
    }

    int ptsfd;
    if (pts_slave[0]) {
        // Opening the TTY has to occur after the
        // fork() and setsid() so that it becomes
        // our controlling TTY and not the daemon's
        ptsfd = open(pts_slave, O_RDWR);
        if (ptsfd == -1) {
            PLOGE("open(pts_slave) daemon");
            exit(-1);
        }

        if (infd < 0)  {
            LOGD("daemon: stdin using PTY");
            infd  = ptsfd;
        }
        if (outfd < 0) {
            LOGD("daemon: stdout using PTY");
            outfd = ptsfd;
        }
        if (errfd < 0) {
            LOGD("daemon: stderr using PTY");
            errfd = ptsfd;
        }
    } else {
        // If a TTY was sent directly, make it the CTTY.
        if (isatty(infd)) {
            ioctl(infd, TIOCSCTTY, 1);
        }
    }
    free(pts_slave);

    return run_daemon_child(infd, outfd, errfd, argc, argv);
}

void redirectStd(int old_fd) {
    dup2(old_fd, STDIN_FILENO);
    dup2(old_fd, STDOUT_FILENO);
    dup2(old_fd, STDERR_FILENO);
}

static void move_cgroup(int dpid) {
    const char* cgroup_path = "/sys/fs/cgroup/uid_0/cgroup.procs";
    char buf[8];
    snprintf(buf, sizeof(buf), "%d\n", dpid);

    if (getuid() != 1000)
        return;

    int fd = open(cgroup_path, O_WRONLY);
    if (fd < 0)
        return;

    if (write(fd, buf, strlen(buf)) == -1) {
        close(fd);
        return;
    }
    close(fd);
}

int run_daemon() {
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

    // The socket path is hidden and won't be accessible via normal filesystem tools
    if (bind(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        PLOGE("daemon bind");
        goto err;
    }

    if (listen(fd, 10) < 0) {
        PLOGE("daemon listen");
        goto err;
    }

    if (fork() != 0) {
        close(fd);
        return 0;
    }

    int client;
    while (1) {
        client = accept(fd, NULL, NULL);
        if (client == -1)
            continue;

        if (fork() == 0) {
            close(fd);
            redirectStd(client);
            return daemon_accept(client);
        }
        move_cgroup(getpid());
        close(client);
    }

    LOGE("daemon exiting");
err:
    close(fd);
    return -1;
}

// List of signals which cause process termination
static int quit_signals[] = { SIGALRM, SIGHUP, SIGPIPE, SIGQUIT, SIGTERM, SIGINT, 0 };

static void sighandler(int sig) {
    LOGE("Caught sig %d", sig);
    restore_stdin();

    // Assume we'll only be called before death
    // See note before sigaction() in set_stdin_raw()
    //
    // Now, close all standard I/O to cause the pumps
    // to exit so we can continue and retrieve the exit
    // code
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Put back all the default handlers
    struct sigaction act;
    int i;

    memset(&act, '\0', sizeof(act));
    act.sa_handler = SIG_DFL;
    for (i = 0; quit_signals[i]; i++) {
        if (sigaction(quit_signals[i], &act, NULL) < 0) {
            PLOGE("Error removing signal handler");
            continue;
        }
    }
}

/**
 * Setup signal handlers trap signals which should result in program termination
 * so that we can restore the terminal to its normal state and retrieve the 
 * return code.
 */
static void setup_sighandlers(void) {
    struct sigaction act;
    int i;

    // Install the termination handlers
    // Note: we're assuming that none of these signal handlers are already trapped.
    // If they are, we'll need to modify this code to save the previous handler and
    // call it after we restore stdin to its previous state.
    memset(&act, '\0', sizeof(act));
    act.sa_handler = &sighandler;
    for (i = 0; quit_signals[i]; i++) {
        if (sigaction(quit_signals[i], &act, NULL) < 0) {
            PLOGE("Error installing signal handler");
            continue;
        }
    }
}

int connect_daemon(int argc, char *argv[], int ppid) {
    int uid = getuid();
    int ptmx;
    char pts_slave[PATH_MAX];

    struct sockaddr_in sun;

    // Open a socket to the daemon
    int socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketfd < 0) {
        PLOGE("socket");
        exit(-1);
    }
    if (fcntl(socketfd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        exit(-1);
    }

    sun.sin_family = AF_INET;
    sun.sin_port = htons(PORT);
    sun.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (0 != connect(socketfd, (struct sockaddr*)&sun, sizeof(sun))) {
        PLOGE("connect");
        exit(-1);
    }

    LOGD("connecting client %d", getpid());

    // Determine which one of our streams are attached to a TTY
    int atty = 0;

    // Send TTYs directly (instead of proxying with a PTY) if
    // the SUPERUSER_SEND_TTY environment variable is set.
    if (getenv("SUPERUSER_SEND_TTY") == NULL) {
        if (isatty(STDIN_FILENO))  atty |= ATTY_IN;
        if (isatty(STDOUT_FILENO)) atty |= ATTY_OUT;
        if (isatty(STDERR_FILENO)) atty |= ATTY_ERR;
    }

    if (atty) {
        // We need a PTY. Get one.
        ptmx = pts_open(pts_slave, sizeof(pts_slave));
        if (ptmx < 0) {
            PLOGE("pts_open");
            exit(-1);
        }
    } else {
        pts_slave[0] = '\0';
    }

    // Send some info to the daemon, starting with our PID
    write_int(socketfd, getpid());
    // Send the slave path to the daemon
    // (This is "" if we're not using PTYs)
    write_string(socketfd, pts_slave);
    // User ID
    write_int(socketfd, uid);
    // Parent PID
    write_int(socketfd, ppid);

    // Send stdin
    if (atty & ATTY_IN) {
        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDIN_FILENO);
    }

    // Send stdout
    if (atty & ATTY_OUT) {
        // Forward SIGWINCH
        watch_sigwinch_async(STDOUT_FILENO, ptmx);

        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDOUT_FILENO);
    }

    // Send stderr
    if (atty & ATTY_ERR) {
        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDERR_FILENO);
    }

    // Number of command line arguments
    write_int(socketfd, argc);

    // Command line arguments
    int i;
    for (i = 0; i < argc; i++) {
        write_string(socketfd, argv[i]);
    }

    // Wait for acknowledgement from daemon
    read_int(socketfd);

    if (atty & ATTY_IN) {
        setup_sighandlers();
        pump_stdin_async(ptmx);
    }
    if (atty & ATTY_OUT) {
        pump_stdout_blocking(ptmx);
    }

    // Get the exit code
    int code = read_int(socketfd);
    close(socketfd);
    LOGD("client exited %d", code);

    return code;
}
