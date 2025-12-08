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

#define MAX_QUEUE 8
#define MAX_NAME 72
#define NUM_PILES 5
#define MAX_PAYLOAD 104

volatile sig_atomic_t active = 1;

/* Player structure */
typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} Player;

/* Game entry for tracking concurrent games */
typedef struct game_entry {
    pid_t pid;
    char p1[MAX_NAME + 1];
    char p2[MAX_NAME + 1];
    struct game_entry *next;
} game_entry_t;

/* Globals */
static Player wait_queue[MAX_QUEUE];
static int wait_count = 0;
static char connected_names[256][MAX_NAME + 1];
static int connected_count = 0;
static game_entry_t *games_head = NULL;
static int listener_fd = -1;

/* ---------- Utilities ---------- */
void log_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap);
}

void handle_signal(int sig)
{
    (void)sig;
    active = 0;
}

void reap_children(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        game_entry_t **pp = &games_head;

        while (*pp) {
            if ((*pp)->pid == pid) {
                log_message("Child %d ended: %s vs %s", pid, (*pp)->p1, (*pp)->p2);

                for (int i = 0; i < connected_count; i++) {
                    if (strcmp(connected_names[i], (*pp)->p1) == 0)
                        connected_names[i][0] = '\0';
                    if (strcmp(connected_names[i], (*pp)->p2) == 0)
                        connected_names[i][0] = '\0';
                }

                game_entry_t *tmp = *pp;
                *pp = tmp->next;
                free(tmp);
                break;
            }

            pp = &(*pp)->next;
        }
    }
}

void setup_signals(void)
{
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    struct sigaction sa_ch;
    sa_ch.sa_handler = reap_children;
    sigemptyset(&sa_ch.sa_mask);
    sa_ch.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_ch, NULL);

    signal(SIGPIPE, SIG_IGN);
}

/* ---------- Low-level IO helpers ---------- */
static ssize_t read_exact(int fd, void *buf, size_t n)
{
    size_t got = 0;
    char *p = (char*)buf;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += r;
    }

    return (ssize_t)got;
}

static ssize_t write_all(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    const char *p = (const char*)buf;

    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += w;
    }

    return (ssize_t)sent;
}

/* ---------- NGP helpers ---------- */
static int send_ngp_message(int fd, const char *type, const char *fmt, ...)
{
    if (strlen(type) != 4)
        return 0;

    char body[256];

    if (!fmt) {
        snprintf(body, sizeof(body), "%s|", type);
    } else {
        va_list ap;
        va_start(ap, fmt);
        char fields[200];
        vsnprintf(fields, sizeof(fields), fmt, ap);
        va_end(ap);
        snprintf(body, sizeof(body), "%s|%s|", type, fields);
    }

    size_t body_len = strlen(body);
    if (body_len > MAX_PAYLOAD)
        return 0;

    char msg[300];
    int hlen = snprintf(msg, sizeof(msg), "0|%02d|", (int)body_len);  // <- cast to int, fix warning
    memcpy(msg + hlen, body, body_len);

    return (write_all(fd, msg, hlen + body_len) == (ssize_t)(hlen + body_len));
}

static char *read_ngp_message(int fd)
{
    char hdr[5];
    if (read_exact(fd, hdr, 5) != 5)
        return NULL;

    if (hdr[0] != '0' || hdr[1] != '|' || !isdigit(hdr[2]) || !isdigit(hdr[3]) || hdr[4] != '|') {
        send_ngp_message(fd, "FAIL", "10 Invalid");
        return NULL;
    }

    int len = (hdr[2]-'0')*10 + (hdr[3]-'0');

    if (len < 4 || len > MAX_PAYLOAD) {
        send_ngp_message(fd, "FAIL", "10 Invalid");
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;

    if (read_exact(fd, buf, len) != len) {
        free(buf);
        send_ngp_message(fd, "FAIL", "10 Invalid");
        return NULL;
    }

    buf[len] = '\0';

    if (buf[len - 1] != '|') {
        free(buf);
        send_ngp_message(fd, "FAIL", "10 Invalid");
        return NULL;
    }

    return buf;
}

static char **split_fields(char *msg, int *count)
{
    int pipes = 0;
    for (char *p = msg; *p; p++)
        if (*p == '|') pipes++;

    char **out = malloc(sizeof(char*) * pipes);
    int idx = 0;
    char *cur = msg;

    while (idx < pipes) {
        char *bar = strchr(cur, '|');
        *bar = '\0';
        out[idx++] = cur;
        cur = bar + 1;
    }

    *count = idx;
    return out;
}

/* ---------- Queue helpers ---------- */
static void enqueue_client(Player p)
{
    if (wait_count >= MAX_QUEUE)
        return;

    wait_queue[wait_count++] = p;
}

static int dequeue_two(Player *a, Player *b)
{
    if (wait_count < 2)
        return 0;

    *a = wait_queue[0];
    *b = wait_queue[1];

    for (int i = 2; i < wait_count; i++)
        wait_queue[i-2] = wait_queue[i];

    wait_count -= 2;
    return 1;
}

static int name_present(const char *name)
{
    for (int i = 0; i < connected_count; i++)
        if (strcmp(connected_names[i], name) == 0)
            return 1;

    return 0;
}

static int add_connected_name(const char *name)
{
    for (int i = 0; i < connected_count; i++) {
        if (connected_names[i][0] == '\0') {
            strncpy(connected_names[i], name, MAX_NAME);
            connected_names[i][MAX_NAME] = '\0';
            return 1;
        }
    }

    if (connected_count < 256) {
        strncpy(connected_names[connected_count], name, MAX_NAME);
        connected_names[connected_count][MAX_NAME] = '\0';
        connected_count++;
        return 1;
    }

    return 0;
}

static void add_game_entry(pid_t pid, const char *p1, const char *p2)
{
    game_entry_t *g = malloc(sizeof(game_entry_t));
    g->pid = pid;
    strncpy(g->p1, p1, MAX_NAME);
    g->p1[MAX_NAME] = '\0';
    strncpy(g->p2, p2, MAX_NAME);
    g->p2[MAX_NAME] = '\0';
    g->next = games_head;
    games_head = g;
}

static int valid_name(const char *s)
{
    if (!s)
        return 0;

    size_t L = strlen(s);
    if (L == 0 || L > MAX_NAME)
        return 0;

    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126 || c == '|')
            return 0;
    }

    return 1;
}

/* ---------- Child: game ---------- */
static void play_game(Player p1, Player p2)
{
    if (listener_fd != -1)
        close(listener_fd);

    int board[NUM_PILES] = {1, 3, 5, 7, 9};
    int turn = 1;
    int winner = 0;
    char buf[128];

    send_ngp_message(p1.fd, "NAME", "1|%s", p2.name);
    send_ngp_message(p2.fd, "NAME", "2|%s", p1.name);

    while (1) {
        snprintf(buf, sizeof(buf), "%d %d %d %d %d",
                 board[0], board[1], board[2], board[3], board[4]);

        send_ngp_message(p1.fd, "PLAY", "%d|%s", turn, buf);
        send_ngp_message(p2.fd, "PLAY", "%d|%s", turn, buf);

        Player *cur = (turn == 1 ? &p1 : &p2);

        char *msg = read_ngp_message(cur->fd);
        if (!msg) {
            winner = (turn == 1 ? 2 : 1);
            break;
        }

        int fc;
        char **fld = split_fields(msg, &fc);

        if (fc < 3 || strcmp(fld[0], "MOVE") != 0) {
            send_ngp_message(cur->fd, "FAIL", "10 Invalid");
            winner = (turn == 1 ? 2 : 1);
            free(msg);
            free(fld);
            break;
        }

        int pile = atoi(fld[1]);
        int qty = atoi(fld[2]);

        free(msg);
        free(fld);

        if (pile < 1 || pile > NUM_PILES) {
            send_ngp_message(cur->fd, "FAIL", "32 Pile Index");
            continue;
        }

        if (qty < 1 || qty > board[pile-1]) {
            send_ngp_message(cur->fd, "FAIL", "33 Quantity");
            continue;
        }

        board[pile-1] -= qty;

        int empty = 1;
        for (int i = 0; i < NUM_PILES; i++)
            if (board[i] > 0)
                empty = 0;

        if (empty) {
            winner = turn;
            break;
        }

        turn = (turn == 1 ? 2 : 1);
    }

    snprintf(buf, sizeof(buf), "%d %d %d %d %d",
             board[0], board[1], board[2], board[3], board[4]);

    send_ngp_message(p1.fd, "OVER", "%d|%s|", winner, buf);
    send_ngp_message(p2.fd, "OVER", "%d|%s|", winner, buf);

    close(p1.fd);
    close(p2.fd);
    _exit(0);
}

/* ---------- Parent: handle OPEN ---------- */
static void handle_open(int fd, char **fld, int fc)
{
    if (fc < 2 || strcmp(fld[0], "OPEN") != 0) {
        send_ngp_message(fd, "FAIL", "10 Invalid");
        close(fd);
        return;
    }

    char *name = fld[1];

    if (!valid_name(name)) {
        send_ngp_message(fd, "FAIL", "21 Long Name");
        close(fd);
        return;
    }

    if (name_present(name)) {
        send_ngp_message(fd, "FAIL", "22 Already Playing");
        close(fd);
        return;
    }

    Player p;
    p.fd = fd;
    strncpy(p.name, name, MAX_NAME);
    p.name[MAX_NAME] = '\0';

    if (!add_connected_name(name)) {
        send_ngp_message(fd, "FAIL", "10 Invalid");
        close(fd);
        return;
    }

    if (!send_ngp_message(fd, "WAIT", NULL)) {
        close(fd);
        return;
    }

    enqueue_client(p);

    if (wait_count >= 2) {
        Player a, b;
        if (dequeue_two(&a, &b)) {
            pid_t pid = fork();
            if (pid < 0) {
                send_ngp_message(a.fd, "FAIL", "10 Invalid");
                send_ngp_message(b.fd, "FAIL", "10 Invalid");
                close(a.fd);
                close(b.fd);
            } else if (pid == 0) {
                play_game(a, b);  // Child handles the game
            } else {
                add_game_entry(pid, a.name, b.name);
                log_message("Started game pid=%d: %s vs %s", pid, a.name, b.name);
                close(a.fd);
                close(b.fd);
            }
        }
    }
}

/* ---------- Main ---------- */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    setup_signals();

    listener_fd = open_listener(argv[1], MAX_QUEUE);
    if (listener_fd < 0) {
        fprintf(stderr, "open_listener failed\n");
        exit(EXIT_FAILURE);
    }

    log_message("Server listening on port %s", argv[1]);

    while (active) {
        struct sockaddr_storage client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        char *msg = read_ngp_message(client_fd);
        if (!msg) {
            close(client_fd);
            continue;
        }

        int fc;
        char **fld = split_fields(msg, &fc);

        if (!fld || fc < 1) {
            send_ngp_message(client_fd, "FAIL", "10 Invalid");
            free(msg);
            free(fld);
            close(client_fd);
            continue;
        }

        handle_open(client_fd, fld, fc);

        free(msg);
        free(fld);
    }

    log_message("Shutting down server");
    close(listener_fd);

    game_entry_t *g = games_head;
    while (g) {
        game_entry_t *tmp = g;
        g = g->next;
        free(tmp);
    }

    return 0;
}
