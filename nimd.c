#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include "network.h"

#define MAX_QUEUE 8 // Max queue size
#define MAX_NAME 72
#define NUM_PILES 5 // Number of piles

volatile sig_atomic_t active = 1; // Flag to control server activity

// Logging message function
void log_message(const char *format, ...) {
    va_list ap;
    va_start(ap, format); // Initialize variable argument list
    vprintf(stdout, format, ap); // Print formatted message to stdout
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap); // Clean up variable argument list
}

// Signal handler to gracefully shut down the server
void handle_signal(int sig) {
    active = 0; // Set active flag to 0 to stop the server loop
}

void reap_children(int sig) {
    int status;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void signal_setup(void) {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL); // Handle SIGINT for graceful shutdown
    sigaction(SIGTERM, &sa, NULL); // Handle SIGTERM for graceful shutdown
    sigaction(SIGHUP, &sa, NULL); // Handle SIGHUP for graceful shutdown

    struct sigaction sa_chld;
    sa_chld.sa_handler = reap_children;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    sigaction(SIGCHLD, &sa_chld, NULL); // Handle SIGCHLD to reap child processes
}

