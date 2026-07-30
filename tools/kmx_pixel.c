/* A pixel pane: an X client on a private display, captured frame by frame.
 *
 * The display is private to the session.  That is not only tidiness - it means
 * a pixel pane cannot draw on whatever screen the operator is actually using,
 * which matters when the machine running this is also somebody's desktop.
 *
 * Capture is ffmpeg's x11grab writing raw RGB to a pipe.  Using a process
 * rather than linking an X library keeps this dependency out of the library
 * and makes the frame source replaceable: anything that can write raw frames
 * to a pipe will do, including the presenter tap once it is wired through. */
#define _GNU_SOURCE

#include "kmx_pixel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define KMX_PIXEL_BYTES 3

static void
close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void
reap(pid_t *pid, unsigned grace_millis) {
    int status;
    unsigned waited = 0;
    if (*pid <= 0) return;
    kill(*pid, SIGTERM);
    while (waited < grace_millis) {
        pid_t result = waitpid(*pid, &status, WNOHANG);
        if (result == *pid || (result < 0 && errno == ECHILD)) {
            *pid = -1;
            return;
        }
        usleep(50000);
        waited += 50;
    }
    if (waitpid(*pid, &status, WNOHANG) != *pid) {
        kill(*pid, SIGKILL);
        (void)waitpid(*pid, &status, 0);
    }
    *pid = -1;
}

/* Displays 40 upward, to stay clear of anything a person is using. */
static int
start_display(int width, int height, pid_t *server) {
    int number;
    for (number = 40; number < 80; number++) {
        char lock[64];
        char geometry[64];
        char name[16];
        pid_t child;
        snprintf(lock, sizeof lock, "/tmp/.X%d-lock", number);
        if (access(lock, F_OK) == 0) continue;
        snprintf(name, sizeof name, ":%d", number);
        snprintf(geometry, sizeof geometry, "%dx%dx24", width, height);
        child = fork();
        if (child < 0) return -1;
        if (child == 0) {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) close(null_fd);
            }
            execlp("Xvfb", "Xvfb", name, "-screen", "0", geometry,
                   "-nolisten", "tcp", (char *)NULL);
            _exit(127);
        }
        /* Wait for the socket rather than sleeping a fixed amount: a busy
         * machine starts it slower and a fast one should not be penalised. */
        {
            char socket_path[64];
            int attempt;
            snprintf(socket_path, sizeof socket_path, "/tmp/.X11-unix/X%d", number);
            for (attempt = 0; attempt < 100; attempt++) {
                int status;
                if (access(socket_path, F_OK) == 0) {
                    *server = child;
                    return number;
                }
                if (waitpid(child, &status, WNOHANG) == child) break;
                usleep(50000);
            }
        }
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    return -1;
}

bool
kmx_pixel_start(
    kmx_pixel_pane *pane,
    const char *command,
    int width,
    int height,
    int fps
) {
    char display_name[16];
    int pipe_fds[2];

    if (!pane || !command || width <= 0 || height <= 0) return false;
    memset(pane, 0, sizeof *pane);
    pane->server = pane->app = pane->capture = -1;
    pane->frames = -1;
    pane->width = width;
    pane->height = height;
    pane->frame_bytes = (size_t)width * (size_t)height * KMX_PIXEL_BYTES;
    pane->frame = malloc(pane->frame_bytes);
    if (!pane->frame) return false;

    pane->display = start_display(width, height, &pane->server);
    if (pane->display < 0) {
        kmx_pixel_stop(pane);
        return false;
    }
    snprintf(display_name, sizeof display_name, ":%d", pane->display);

    pane->app = fork();
    if (pane->app < 0) {
        kmx_pixel_stop(pane);
        return false;
    }
    if (pane->app == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        setenv("DISPLAY", display_name, 1);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    if (pipe(pipe_fds) != 0) {
        kmx_pixel_stop(pane);
        return false;
    }
    pane->capture = fork();
    if (pane->capture < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        kmx_pixel_stop(pane);
        return false;
    }
    if (pane->capture == 0) {
        char geometry[64];
        char rate[16];
        int null_fd = open("/dev/null", O_RDWR);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        if (getenv("KMX_DEBUG_FRAMES")) {
            /* Let the capture explain itself when asked. */
            int tty = open("/dev/tty", O_WRONLY);
            if (tty >= 0) {
                dup2(tty, STDERR_FILENO);
                if (tty > STDERR_FILENO) close(tty);
            }
        }
        snprintf(geometry, sizeof geometry, "%dx%d", width, height);
        snprintf(rate, sizeof rate, "%d", fps > 0 ? fps : 10);
        execlp("ffmpeg", "ffmpeg", "-loglevel",
               getenv("KMX_DEBUG_FRAMES") ? "error" : "quiet",
               "-f", "x11grab", "-video_size", geometry,
               "-framerate", rate, "-i", display_name,
               "-f", "rawvideo", "-pix_fmt", "rgb24", "-", (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    pane->frames = pipe_fds[0];
    {
        int flags = fcntl(pane->frames, F_GETFL, 0);
        if (flags >= 0) fcntl(pane->frames, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

bool
kmx_pixel_poll(kmx_pixel_pane *pane, bool *closed) {
    if (closed) *closed = false;
    if (!pane || pane->frames < 0) {
        if (closed) *closed = true;
        return false;
    }
    while (pane->filled < pane->frame_bytes) {
        ssize_t count = read(
            pane->frames,
            pane->frame + pane->filled,
            pane->frame_bytes - pane->filled);
        if (count > 0) {
            pane->filled += (size_t)count;
            continue;
        }
        if (count == 0) {
            if (closed) *closed = true;
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        if (closed) *closed = true;
        return false;
    }
    /* A whole frame; the next call starts the one after it. */
    pane->filled = 0;
    return true;
}

void
kmx_pixel_stop(kmx_pixel_pane *pane) {
    if (!pane) return;
    close_fd(&pane->frames);
    reap(&pane->capture, 250);
    /* GUI applications keep profile state. Give them time to flush it after
     * SIGTERM instead of turning every streamed session into a crash restore. */
    reap(&pane->app, 5000);
    reap(&pane->server, 250);
    free(pane->frame);
    pane->frame = NULL;
}
