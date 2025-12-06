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
#define MAX_PAYLOAD 99

volatile sig_atomic_t active = 1;

/* Structures for queued players and active games */
typedef struct {
    int fd;
    char name[MAX_NAME + 1];
    int opened; 
} Player;

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

void log_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap);
}

void handle_signal(int sig) {
    (void)sig;
    active = 0;
}

static int add_connected_name(const char *name);
static void remove_connected_name(const char *name);

/* Reap children and remove names from connected list */
void reap_children(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        game_entry_t **pp = &games_head;

        while (*pp) {
            if ((*pp)->pid == pid) {
                log_message("Child %d ended: %s vs %s", pid, (*pp)->p1, (*pp)->p2);
                remove_connected_name((*pp)->p1);
                remove_connected_name((*pp)->p2);

                game_entry_t *tmp = *pp;
                *pp = tmp->next;
                free(tmp);
                break;
            }
            pp = &(*pp)->next;
        }
    }
}

void setup_signals(void) {
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

static ssize_t read_exact(int fd, void *buf, size_t n) {
    size_t got = 0;
    char *p = (char*)buf;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }

    return (ssize_t)got;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    const char *p = (const char*)buf;

    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t) w;
    }

    return (ssize_t)sent;
}

/* ---------- NGP framing helpers ---------- */

static char *read_ngp_payload(int fd) {
    char header[5];
    ssize_t r = read_exact(fd, header, 5);

    if (r <= 0) return NULL;

    if (!(header[0] == '0' && header[1] == '|' &&
          isdigit((unsigned char)header[2]) && isdigit((unsigned char)header[3]) &&
          header[4] == '|')) {

        const char *fail_payload = "FAIL|10 Invalid|";
        char hbuf[16];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail_payload));
        write_all(fd, hbuf, strlen(hbuf));
        write_all(fd, fail_payload, strlen(fail_payload));
        return NULL;
    }

    int payload_len = (header[2]-'0')*10 + (header[3]-'0');

    if (payload_len < 0 || payload_len > MAX_PAYLOAD) {
        const char *fail_payload = "FAIL|10 Invalid|";
        char hbuf[16];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail_payload));
        write_all(fd, hbuf, strlen(hbuf));
        write_all(fd, fail_payload, strlen(fail_payload));
        return NULL;
    }

    char *payload = malloc((size_t)payload_len + 1);

    if (!payload) return NULL;

    if (payload_len == 0) {
        payload[0] = '\0';
        return payload;
    }

    ssize_t rr = read_exact(fd, payload, (size_t)payload_len);

    if (rr <= 0) {
        free(payload);
        return NULL;
    }

    payload[payload_len] = '\0';

    if (payload[payload_len - 1] != '|') {
        const char *fail_payload = "FAIL|10 Invalid|";
        char hbuf[16];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail_payload));
        write_all(fd, hbuf, strlen(hbuf));
        write_all(fd, fail_payload, strlen(fail_payload));
        free(payload);
        return NULL;
    }

    return payload;
}

static char **split_payload(char *payload, int *out_count) {
    if (!payload) {
        *out_count = 0;
        return NULL;
    }

    int pipes = 0;

    for (char *p = payload; *p; ++p) {
        if (*p == '|') ++pipes;
    }

    if (pipes <= 0) {
        *out_count = 0;
        return NULL;
    }

    char **arr = malloc((pipes + 1) * sizeof(char*));

    if (!arr) return NULL;

    int idx = 0;
    char *cur = payload;

    while (idx < pipes) {
        arr[idx++] = cur;
        char *bar = strchr(cur, '|');
        if (!bar) break;
        *bar = '\0';
        cur = bar + 1;
    }

    if (idx < pipes) arr[idx++] = cur;
    *out_count = idx;

    return arr;
}

static int send_ngp_message(int fd, const char *type, const char *format, ...) {
    char payload[MAX_PAYLOAD + 1];

    if (!format || format[0] == '\0') {
        int n = snprintf(payload, sizeof(payload), "%s|", type);
        if (n < 0 || n > MAX_PAYLOAD) return 0;
    } else {
        va_list ap;
        va_start(ap, format);
        char fields[MAX_PAYLOAD + 1];
        int fld_len = vsnprintf(fields, sizeof(fields), format, ap);
        va_end(ap);

        if (fld_len < 0 || fld_len > (int)MAX_PAYLOAD) return 0;

        int n = snprintf(payload, sizeof(payload), "%s|%s|", type, fields);
        if (n < 0 || n > MAX_PAYLOAD) return 0;
    }

    size_t payload_len = strlen(payload);

    if (payload_len > (size_t)MAX_PAYLOAD) return 0;

    char header[16];
    int hlen = snprintf(header, sizeof(header), "0|%02zu|", payload_len);
    if (hlen < 0) return 0;

    size_t total = (size_t)hlen + payload_len;
    char *buf = malloc(total);

    if (!buf) return 0;

    memcpy(buf, header, (size_t)hlen);
    memcpy(buf + hlen, payload, payload_len);

    ssize_t w = write_all(fd, buf, total);
    free(buf);

    return (w == (ssize_t)total) ? 1 : 0;
}

/* ---------- Helpers for connected names & queue ---------- */

static void enqueue_client(Player p) {
    if (wait_count >= MAX_QUEUE) return;
    wait_queue[wait_count++] = p;
}

static int dequeue_two(Player *a, Player *b) {
    if (wait_count < 2) return 0;
    *a = wait_queue[0];
    *b = wait_queue[1];
    for (int i = 2; i < wait_count; ++i) {
        wait_queue[i - 2] = wait_queue[i];
    }
    wait_count -= 2;
    return 1;
}

static int name_present(const char *name) {
    for (int i = 0; i < connected_count; ++i) {
        if (connected_names[i][0] && strcmp(connected_names[i], name) == 0) return 1;
    }
    return 0;
}

static int add_connected_name(const char *name) {
    int max = (int)(sizeof(connected_names) / sizeof(connected_names[0]));
    for (int i = 0; i < connected_count; ++i) {
        if (connected_names[i][0] == '\0') {
            strncpy(connected_names[i], name, MAX_NAME);
            connected_names[i][MAX_NAME] = '\0';
            return 1;
        }
    }
    if (connected_count < max) {
        strncpy(connected_names[connected_count], name, MAX_NAME);
        connected_names[connected_count][MAX_NAME] = '\0';
        connected_count++;
        return 1;
    }
    return 0;
}

static void remove_connected_name(const char *name) {
    for (int i = 0; i < connected_count; ++i) {
        if (connected_names[i][0] && strcmp(connected_names[i], name) == 0) {
            connected_names[i][0] = '\0';
            return;
        }
    }
}

static void add_game_entry(pid_t pid, const char *p1, const char *p2) {
    game_entry_t *g = malloc(sizeof(game_entry_t));
    if (!g) return;
    g->pid = pid;
    strncpy(g->p1, p1, MAX_NAME);
    g->p1[MAX_NAME] = '\0';
    strncpy(g->p2, p2, MAX_NAME);
    g->p2[MAX_NAME] = '\0';
    g->next = games_head;
    games_head = g;
}

static int valid_name(const char *s) {
    if (!s) return 0;
    size_t L = strlen(s);
    if (L == 0 || L > MAX_NAME) return 0;
    for (size_t i = 0; i < L; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126 || c == '|') return 0;
    }
    return 1;
}

/* ---------- Game logic (child) ---------- */

static void board_info(int *board, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%d %d %d %d %d",
             board[0], board[1], board[2], board[3], board[4]);
}

static int check_game_over_board(int *board) {
    for (int i = 0; i < NUM_PILES; ++i) {
        if (board[i] > 0) return 0;
    }
    return 1;
}

static void play_game(Player p1, Player p2) {
    if (listener_fd != -1) close(listener_fd);

    int board[NUM_PILES] = {1, 3, 5, 7, 9};
    int turn = 0;
    int winner = -1;
    char board_buf[128];

    send_ngp_message(p1.fd, "NAME", "1|%s", p2.name);
    send_ngp_message(p2.fd, "NAME", "2|%s", p1.name);

    while (!check_game_over_board(board)) {
        Player *cur = (turn == 0) ? &p1 : &p2;

        board_info(board, board_buf, sizeof(board_buf));
        send_ngp_message(p1.fd, "PLAY", "%d|%s", turn + 1, board_buf);
        send_ngp_message(p2.fd, "PLAY", "%d|%s", turn + 1, board_buf);

        char *payload = read_ngp_payload(cur->fd);
        if (!payload) {
            winner = (turn == 0) ? 2 : 1;
            break;
        }

        int fldc = 0;
        char **flds = split_payload(payload, &fldc);
        if (!flds || fldc < 1) {
            send_ngp_message(cur->fd, "FAIL", "10 Invalid");
            free(payload);
            free(flds);
            winner = (turn == 0) ? 2 : 1;
            break;
        }

        char *type = flds[0];

        if (strcmp(type, "MOVE") != 0) {
            send_ngp_message(cur->fd, "FAIL", "10 Invalid");
            free(payload);
            free(flds);
            winner = (turn == 0) ? 2 : 1;
            break;
        }

        if (fldc < 3) {
            send_ngp_message(cur->fd, "FAIL", "10 Invalid");
            free(payload);
            free(flds);
            winner = (turn == 0) ? 2 : 1;
            break;
        }

        int pile = atoi(flds[1]);
        int qty  = atoi(flds[2]);

        free(payload);
        free(flds);

        if (pile < 1 || pile > NUM_PILES) {
            send_ngp_message(cur->fd, "FAIL", "32 Pile Index");
            continue;
        }

        int idx = pile - 1;

        if (qty < 1 || qty > board[idx]) {
            send_ngp_message(cur->fd, "FAIL", "33 Quantity");
            continue;
        }

        board[idx] -= qty;

        log_message("%s removed %d from pile %d -> board: %d %d %d %d %d",
                    (turn == 0) ? p1.name : p2.name,
                    qty,
                    pile,
                    board[0], board[1], board[2], board[3], board[4]);

        if (check_game_over_board(board)) {
            winner = turn + 1;
            break;
        }

        turn = 1 - turn;
    }

    board_info(board, board_buf, sizeof(board_buf));
    if (winner < 1) winner = 0;

    send_ngp_message(p1.fd, "OVER", "%d|%s", winner, board_buf);
    send_ngp_message(p2.fd, "OVER", "%d|%s", winner, board_buf);

    close(p1.fd);
    close(p2.fd);
    _exit(EXIT_SUCCESS);
}

/* ---------- Parent: initial OPEN handling ---------- */

static void handle_open_message(int client_fd, char **flds, int fldc, int opened_before) {
    if (fldc < 2) {
        send_ngp_message(client_fd, "FAIL", "10 Invalid");
        close(client_fd);
        return;
    }

    char *type = flds[0];

    if (strcmp(type, "OPEN") != 0) {
        if (strcmp(type, "MOVE") == 0) {
            send_ngp_message(client_fd, "FAIL", "24 Not Playing");
            close(client_fd);
            return;
        }
        send_ngp_message(client_fd, "FAIL", "10 Invalid");
        close(client_fd);
        return;
    }

    char *name = flds[1];

    if (!valid_name(name)) {
        if (strlen(name) > MAX_NAME) {
            send_ngp_message(client_fd, "FAIL", "21 Long Name");
        } else {
            send_ngp_message(client_fd, "FAIL", "10 Invalid");
        }
        close(client_fd);
        return;
    }

    if (name_present(name)) {
        send_ngp_message(client_fd, "FAIL", "22 Already Playing");
        close(client_fd);
        return;
    }

    if (opened_before) {
        send_ngp_message(client_fd, "FAIL", "23 Already Open");
        close(client_fd);
        return;
    }

    Player p;
    p.fd = client_fd;
    strncpy(p.name, name, MAX_NAME);
    p.name[MAX_NAME] = '\0';
    p.opened = 1;

    if (!add_connected_name(name)) {
        send_ngp_message(client_fd, "FAIL", "10 Invalid");
        close(client_fd);
        return;
    }

    if (!send_ngp_message(client_fd, "WAIT", NULL)) {
        close(client_fd);
        remove_connected_name(p.name);
        return;
    }

    enqueue_client(p);
    log_message("Enqueued player '%s' (fd=%d)", p.name, p.fd);

    Player a, b;
    if (dequeue_two(&a, &b)) {
        pid_t pid = fork();
        if (pid < 0) {
            send_ngp_message(a.fd, "FAIL", "10 Invalid");
            send_ngp_message(b.fd, "FAIL", "10 Invalid");
            close(a.fd);
            close(b.fd);
            remove_connected_name(a.name);
            remove_connected_name(b.name);
        } else if (pid == 0) {
            play_game(a, b);
        } else {
            add_game_entry(pid, a.name, b.name);
            log_message("Started game pid=%d: %s vs %s", pid, a.name, b.name);
            close(a.fd);
            close(b.fd);
        }
    }
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
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
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char *payload = read_ngp_payload(client_fd);
        if (!payload) {
            close(client_fd);
            continue;
        }

        int fldc = 0;
        char **flds = split_payload(payload, &fldc);
        if (!flds || fldc < 1) {
            send_ngp_message(client_fd, "FAIL", "10 Invalid");
            free(payload);
            free(flds);
            close(client_fd);
            continue;
        }

        int opened_before = 0;
        for (int i = 0; i < wait_count; ++i) {
            if (wait_queue[i].fd == client_fd) opened_before = 1;
        }

        handle_open_message(client_fd, flds, fldc, opened_before);

        free(payload);
        free(flds);
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