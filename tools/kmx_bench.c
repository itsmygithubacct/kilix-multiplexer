/* kmx-bench — what the cell plane actually costs.
 *
 * Runs a real program under a real PTY, feeds its output through the
 * synchroniser at a fixed cadence, and reports what would have gone on the
 * wire.  Real programs rather than synthetic streams, because the claim being
 * tested is about vim and shells, not about a generator's idea of them.
 *
 *   kmx-bench [--rows N] [--cols N] [--interval MS] [--label NAME] -- CMD [ARG...]
 *
 * The comparison figure is the raw PTY byte count, which is what the tmux tier
 * would have carried for the same workload. */
#define _GNU_SOURCE

#include "kilix_mux.h"

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static uint64_t
now_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static double
cpu_seconds(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
           (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

static void
usage(void) {
    fputs(
        "usage: kmx-bench [--rows N] [--cols N] [--interval MS] [--label NAME]"
        " -- COMMAND [ARG...]\n", stderr);
}

int
main(int argc, char **argv) {
    struct winsize size;
    kmx_sync *sync = NULL;
    kmx_receiver *receiver = NULL;
    unsigned char buffer[65536];
    const char *label = "workload";
    int rows = 24;
    int cols = 80;
    int interval = KMX_SEND_INTERVAL_MIN_MS;
    int index = 1;
    int master = -1;
    pid_t child;
    uint64_t started;
    uint64_t elapsed;
    size_t pty_bytes = 0;
    size_t wire_bytes = 0;
    size_t raw_bytes = 0;
    size_t messages = 0;
    size_t largest = 0;
    double cpu_start;
    double cpu_used;
    bool running = true;

    while (index < argc && strcmp(argv[index], "--") != 0) {
        if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--cols") == 0 && index + 1 < argc) {
            cols = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--interval") == 0 && index + 1 < argc) {
            interval = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--label") == 0 && index + 1 < argc) {
            label = argv[++index];
        } else {
            usage();
            return 2;
        }
        index++;
    }
    if (index >= argc || strcmp(argv[index], "--") != 0 || index + 1 >= argc) {
        usage();
        return 2;
    }
    index++;
    if (rows <= 0 || cols <= 0) {
        usage();
        return 2;
    }

    if (kmx_sync_create(&sync, rows, cols) != KMX_OK ||
        kmx_receiver_create(&receiver, rows, cols) != KMX_OK) {
        fprintf(stderr, "kmx-bench: out of memory\n");
        return 1;
    }
    kmx_sync_set_interval(sync, (unsigned)interval);

    memset(&size, 0, sizeof size);
    size.ws_row = (unsigned short)rows;
    size.ws_col = (unsigned short)cols;
    child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        fprintf(stderr, "kmx-bench: forkpty: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        execvp(argv[index], &argv[index]);
        _exit(127);
    }

    started = now_millis();
    cpu_start = cpu_seconds();
    while (running) {
        struct pollfd descriptor = {.fd = master, .events = POLLIN, .revents = 0};
        kmx_buffer message;
        kmx_sync_info info;
        bool produced = false;
        int ready = poll(&descriptor, 1, interval);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready > 0 && (descriptor.revents & POLLIN)) {
            ssize_t count = read(master, buffer, sizeof buffer);
            if (count > 0) {
                pty_bytes += (size_t)count;
                if (kmx_sync_feed(sync, buffer, (size_t)count) != KMX_OK) break;
            } else if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN)) {
                /* On Linux the master reads EIO once the last slave closes. */
                running = false;
            }
        } else if (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR))) {
            /* The child is gone and there is nothing left to drain.  Without
             * this the loop spins on an immediately-returning poll. */
            running = false;
        } else if (ready == 0) {
            int status;
            if (waitpid(child, &status, WNOHANG) == child) running = false;
        }

        kmx_buffer_init(&message);
        if (kmx_sync_poll(sync, now_millis(), &message, &produced, &info) != KMX_OK) {
            kmx_buffer_free(&message);
            break;
        }
        if (produced) {
            uint64_t sequence = 0;
            messages++;
            wire_bytes += message.size;
            raw_bytes += info.raw_bytes;
            if (message.size > largest) largest = message.size;
            /* Apply and acknowledge immediately: this measures the encoding,
             * not a particular network's behaviour. */
            if (kmx_receiver_apply(
                    receiver, message.data, message.size, &sequence) != KMX_OK) {
                fprintf(stderr, "kmx-bench: receiver rejected a message\n");
                kmx_buffer_free(&message);
                break;
            }
            kmx_sync_ack(sync, sequence);
        }
        kmx_buffer_free(&message);
    }
    elapsed = now_millis() - started;
    cpu_used = cpu_seconds() - cpu_start;

    /* Drain anything still pending so the final screen is measured too. */
    {
        kmx_buffer message;
        kmx_sync_info info;
        bool produced = false;
        kmx_buffer_init(&message);
        if (kmx_sync_poll(sync, now_millis() + KMX_SEND_INTERVAL_MAX_MS,
                          &message, &produced, &info) == KMX_OK && produced) {
            uint64_t sequence = 0;
            messages++;
            wire_bytes += message.size;
            raw_bytes += info.raw_bytes;
            kmx_receiver_apply(receiver, message.data, message.size, &sequence);
            kmx_sync_ack(sync, sequence);
        }
        kmx_buffer_free(&message);
    }

    close(master);
    {
        int status;
        waitpid(child, &status, 0);
    }

    /* The convergence check is part of the measurement: a cheap encoding that
     * did not actually reproduce the screen would not be worth reporting. */
    {
        bool converged = kmx_grid_equal(
            kmx_receiver_grid(receiver), kmx_sync_current(sync));
        if (!elapsed) elapsed = 1;
        printf("{\n");
        printf("  \"label\": \"%s\",\n", label);
        printf("  \"rows\": %d, \"cols\": %d, \"interval_ms\": %d,\n",
               rows, cols, interval);
        printf("  \"elapsed_ms\": %llu,\n", (unsigned long long)elapsed);
        printf("  \"pty_bytes\": %zu,\n", pty_bytes);
        printf("  \"wire_bytes\": %zu,\n", wire_bytes);
        printf("  \"encoded_bytes\": %zu,\n", raw_bytes);
        printf("  \"messages\": %zu,\n", messages);
        printf("  \"largest_message\": %zu,\n", largest);
        printf("  \"wire_bytes_per_second\": %.1f,\n",
               (double)wire_bytes * 1000.0 / (double)elapsed);
        printf("  \"pty_bytes_per_second\": %.1f,\n",
               (double)pty_bytes * 1000.0 / (double)elapsed);
        printf("  \"reduction_factor\": %.2f,\n",
               wire_bytes ? (double)pty_bytes / (double)wire_bytes : 0.0);
        printf("  \"cpu_seconds\": %.3f,\n", cpu_used);
        printf("  \"converged\": %s\n", converged ? "true" : "false");
        printf("}\n");
        kmx_sync_free(sync);
        kmx_receiver_free(receiver);
        return converged ? 0 : 1;
    }
}
