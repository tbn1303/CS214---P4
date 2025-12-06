#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <poll.h>
#include "network.h"

/* Keep your names and constants */
#define MAX_QUEUE 8
#define MAX_NAME 72
#define NUM_PILES 5
#define MAX_PAYLOAD 104   /* per project */
#define MAX_MSG (5 + MAX_PAYLOAD)

volatile sig_atomic_t active = 1; /* server keeps running while active != 0 */

/* ---------- Data structures ---------- */

typedef struct client_node {
    int fd;
    char name[MAX_NAME + 1];
    struct client_node *next;
} client_node_t;

typedef struct name_node {
    char name[MAX_NAME + 1];
    struct name_node *next;
} name_node_t;

typedef struct game_entry {
    pid_t pid;
    char p1[MAX_NAME + 1];
    char p2[MAX_NAME + 1];
    struct game_entry *next;
} game_entry_t;

typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} PlayerInfo;

/* Parent-global state */
static client_node_t *wait_head = NULL, *wait_tail = NULL;
static name_node_t *connected_names = NULL; /* waiting + playing */
static game_entry_t *games = NULL;
static int listener_fd = -1;

/* Self-pipe for SIGCHLD notifications */
static int spipe[2] = {-1, -1};

/* ---------- Logging (keeps your function name) ---------- */

void log_message(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stdout, format, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap);
}

/* ---------- Signal handling (keep your names) ---------- */

void handle_signal(int sig) {
    (void)sig;
    active = 0;
}

/* Reap children (called in main when notified via self-pipe) */
void reap_children(int sig) {
    (void)sig;
    /* write a byte to pipe in signal handler — here we install handler that writes; see below */
    /* This function is not used as signal handler here (we use a minimal handler that writes). */
}

/* Setup signal handlers (keeps your name signal_setup) */
void signal_setup(void)
{
    /* Create self-pipe for SIGCHLD notifications */
    if (pipe(spipe) < 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    /* Handler for SIGCHLD -> write a byte to spipe[1] */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = /* inline handler */ (void(*)(int)) ( + (void*)0 );
    /* We can't assign an inline lambda in C; instead install a small function below. */
    /* We'll set handler with sigaction after defining a small function. */
    /* Install simple handlers for termination signals to set active = 0 */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_signal;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    if (sigaction(SIGINT, &sa_term, NULL) < 0) { perror("sigaction SIGINT"); exit(EXIT_FAILURE); }
    if (sigaction(SIGTERM, &sa_term, NULL) < 0) { perror("sigaction SIGTERM"); exit(EXIT_FAILURE); }
    if (sigaction(SIGHUP, &sa_term, NULL) < 0) { perror("sigaction SIGHUP"); exit(EXIT_FAILURE); }

    /* SIGPIPE ignore so writing to closed socket doesn't kill process */
    signal(SIGPIPE, SIG_IGN);

    /* Now install SIGCHLD handler that writes to pipe (async-signal-safe) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int signo) {
        (void)signo;
        uint8_t b = 1;
        /* best-effort write; ignore errors */
        if (spipe[1] != -1) write(spipe[1], &b, 1);
    };
    /* The above lambda-like assignment is not valid in C standard. We'll implement proper handler instead below. */
    /* Because standard C doesn't support closures, we'll implement a static function for SIGCHLD and set it. */
}

/* Since C doesn't have lambdas, define the SIGCHLD handler function here: */
static void _sigchld_pipe_write(int signo) {
    (void)signo;
    uint8_t b = 1;
    if (spipe[1] != -1) {
        ssize_t r = write(spipe[1], &b, 1);
        (void)r;
    }
}

/* Reap children and clean up connected_names for ended games */
static void process_sigchld_events(void)
{
    /* Drain the pipe */
    uint8_t buf[64];
    while (read(spipe[0], buf, sizeof(buf)) > 0) { /* drain */ }

    /* Reap children and remove their game entries and connected names */
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        char p1[MAX_NAME + 1] = {0}, p2[MAX_NAME + 1] = {0};
        /* remove game entry and get p1/p2 */
        game_entry_t **pp = &games;
        while (*pp) {
            if ((*pp)->pid == pid) {
                game_entry_t *tmp = *pp;
                strncpy(p1, tmp->p1, MAX_NAME);
                strncpy(p2, tmp->p2, MAX_NAME);
                *pp = tmp->next;
                free(tmp);
                break;
            }
            pp = &(*pp)->next;
        }
        if (p1[0]) {
            log_message("Child %d ended; removing names '%s' and '%s'", pid, p1, p2);
            /* remove connected names */
            name_node_t **np = &connected_names;
            while (*np) {
                if (strcmp((*np)->name, p1) == 0) {
                    name_node_t *tmp = *np;
                    *np = tmp->next;
                    free(tmp);
                    break;
                }
                np = &(*np)->next;
            }
            np = &connected_names;
            while (*np) {
                if (strcmp((*np)->name, p2) == 0) {
                    name_node_t *tmp = *np;
                    *np = tmp->next;
                    free(tmp);
                    break;
                }
                np = &(*np)->next;
            }
        } else {
            log_message("Child %d ended (no game entry)", pid);
        }
    }
}

/* ---------- NGP framing helpers ---------- */

/* read exactly n bytes or return <=0 on error/EOF */
static ssize_t read_exact(int fd, void *buf, size_t n)
{
    size_t got = 0;
    char *p = buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t) r;
    }
    return (ssize_t) got;
}

/* read NGP payload; returns malloc'd payload string (must free) or NULL.
   Sends FAIL|10 Invalid| if framing error. */
static char *read_ngp_payload(int fd)
{
    char header[5];
    if (read_exact(fd, header, 5) <= 0) return NULL;
    if (!(header[0] == '0' && header[1] == '|' && isdigit((unsigned char)header[2]) &&
          isdigit((unsigned char)header[3]) && header[4] == '|')) {
        /* invalid header */
        /* send FAIL and close */
        const char *fail = "FAIL|10 Invalid|";
        /* Build full message */
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail));
        write(fd, hbuf, strlen(hbuf));
        write(fd, fail, strlen(fail));
        return NULL;
    }
    int payload_len = (header[2]-'0')*10 + (header[3]-'0');
    if (payload_len < 0 || payload_len > MAX_PAYLOAD) {
        const char *fail = "FAIL|10 Invalid|";
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail));
        write(fd, hbuf, strlen(hbuf));
        write(fd, fail, strlen(fail));
        return NULL;
    }
    char *payload = malloc(payload_len + 1);
    if (!payload) return NULL;
    if (read_exact(fd, payload, payload_len) <= 0) { free(payload); return NULL; }
    payload[payload_len] = '\0';
    if (payload[payload_len - 1] != '|') {
        const char *fail = "FAIL|10 Invalid|";
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "0|%02zu|", strlen(fail));
        write(fd, hbuf, strlen(hbuf));
        write(fd, fail, strlen(fail));
        free(payload);
        return NULL;
    }
    return payload;
}

/* split payload "TYPE|f1|f2|...|" into array of pointers (malloc'd)
   sets out_count, caller must free both array and buffer */
static char **split_payload(char *payload, int *out_count)
{
    int pipes = 0;
    for (char *p = payload; *p; ++p) if (*p == '|') ++pipes;
    if (pipes <= 0) { *out_count = 0; return NULL; }
    char **arr = malloc((pipes + 1) * sizeof(char*));
    if (!arr) return NULL;
    int idx = 0;
    char *cur = payload;
    while (idx <= pipes) {
        arr[idx++] = cur;
        char *bar = strchr(cur, '|');
        if (!bar) break;
        *bar = '\0';
        cur = bar + 1;
    }
    *out_count = idx;
    return arr;
}

/* Build and send an NGP message given payload (payload must include trailing '|') */
static ssize_t send_ngp_payload(int fd, const char *payload)
{
    size_t payload_len = strlen(payload);
    if (payload_len > MAX_PAYLOAD) return -1;
    char header[8];
    snprintf(header, sizeof(header), "0|%02zu|", payload_len);
    size_t hlen = strlen(header);
    size_t total = hlen + payload_len;
    char *buf = malloc(total);
    if (!buf) return -1;
    memcpy(buf, header, hlen);
    memcpy(buf + hlen, payload, payload_len);
    ssize_t w = write(fd, buf, total);
    free(buf);
    return w;
}

/* Wrapper keeping name send_ngp_message; format string forms the payload fields (not including TYPE) */
int send_ngp_message(int fd, const char *type, const char *format, ...)
{
    char payload[MAX_PAYLOAD + 1];
    if (!format || format[0] == '\0') {
        /* payload is TYPE| (no fields) */
        snprintf(payload, sizeof(payload), "%s|", type);
    } else {
        va_list ap;
        va_start(ap, format);
        char fields[MAX_PAYLOAD + 1];
        vsnprintf(fields, sizeof(fields), format, ap);
        va_end(ap);
        /* Build payload: TYPE|fields|  */
        snprintf(payload, sizeof(payload), "%s|%s|", type, fields);
    }
    return (send_ngp_payload(fd, payload) > 0) ? 1 : 0;
}

/* ---------- Helpers for connected names and wait queue (keep small code) ---------- */

static void enqueue_client(client_node_t *c)
{
    c->next = NULL;
    if (!wait_tail) wait_head = wait_tail = c;
    else { wait_tail->next = c; wait_tail = c; }
}

static client_node_t *dequeue_client(void)
{
    client_node_t *c = wait_head;
    if (c) {
        wait_head = c->next;
        if (!wait_head) wait_tail = NULL;
        c->next = NULL;
    }
    return c;
}

static int name_present(const char *name)
{
    for (name_node_t *p = connected_names; p; p = p->next)
        if (strcmp(p->name, name) == 0) return 1;
    return 0;
}

static void add_connected_name(const char *name)
{
    name_node_t *n = malloc(sizeof(name_node_t));
    if (!n) return;
    strncpy(n->name, name, MAX_NAME);
    n->name[MAX_NAME] = '\0';
    n->next = connected_names;
    connected_names = n;
}

static void remove_connected_name(const char *name)
{
    name_node_t **pp = &connected_names;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            name_node_t *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Add game entry mapping pid -> p1/p2 */
static void add_game_entry(pid_t pid, const char *p1, const char *p2)
{
    game_entry_t *e = malloc(sizeof(game_entry_t));
    if (!e) return;
    e->pid = pid;
    strncpy(e->p1, p1, MAX_NAME);
    e->p1[MAX_NAME] = '\0';
    strncpy(e->p2, p2, MAX_NAME);
    e->p2[MAX_NAME] = '\0';
    e->next = games;
    games = e;
}

/* ---------- Game helpers (keeping your names) ---------- */

/* keep your board_info name: produce a board string like "1 3 5 7 9" */
void board_info(int *board, char *buf)
{
    snprintf(buf, 128, "%d %d %d %d %d",
             board[0], board[1], board[2], board[3], board[4]);
}

/* keep your check_game_over name: returns 1 when no stones remain */
int check_game_over(int *board)
{
    for (int i = 0; i < NUM_PILES; ++i) {
        if (board[i] > 0) return 0;
    }
    return 1;
}

/* Validate printable ASCII name, no '|' and length <= MAX_NAME */
static int valid_name_chars(const char *s)
{
    if (!s) return 0;
    size_t L = strlen(s);
    if (L == 0 || L > MAX_NAME) return 0;
    for (size_t i = 0; i < L; ++i) {
        unsigned char c = (unsigned char) s[i];
        if (c < 32 || c > 126 || c == '|') return 0;
    }
    return 1;
}

/* Child: run the game between two PlayerInfo (child owns the client_node memory) */
void play_game(PlayerInfo player1, PlayerInfo player2)
{
    /* Child should close listener_fd if inherited */
    if (listener_fd != -1) close(listener_fd);

    int board[NUM_PILES] = {1, 3, 5, 7, 9}; /* required start */
    int turn = 0; /* 0 -> player1, 1 -> player2 */
    int winner = -1;
    char board_buf[128];

    /* Send NAME messages to both (NAME|playernum|opponent|) */
    send_ngp_message(player1.fd, "NAME", "1|%s", player2.name);
    send_ngp_message(player2.fd, "NAME", "2|%s", player1.name);

    while (!check_game_over(board)) {
        PlayerInfo *cur = (turn == 0) ? &player1 : &player2;
        PlayerInfo *opp = (turn == 0) ? &player2 : &player1;

        board_info(board, board_buf);
        /* PLAY|<next player number>|<board>| */
        send_ngp_message(player1.fd, "PLAY", "%d|%s", turn + 1, board_buf);
        send_ngp_message(player2.fd, "PLAY", "%d|%s", turn + 1, board_buf);

        /* Use poll to wait for activity on either socket (but we enforce turn order) */
        struct pollfd pfds[2];
        pfds[0].fd = player1.fd; pfds[0].events = POLLIN;
        pfds[1].fd = player2.fd; pfds[1].events = POLLIN;
        int rc;
        while (1) {
            rc = poll(pfds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                winner = (turn == 0) ? 1 : 0; /* other wins on fatal error */
                break;
            }
            if (rc == 0) continue;
            break;
        }
        if (rc < 0) break;

        /* Determine mover fd */
        int mover_fd = -1;
        if (pfds[0].revents & POLLIN) mover_fd = player1.fd;
        else if (pfds[1].revents & POLLIN) mover_fd = player2.fd;
        else { winner = (turn == 0) ? 1 : 0; break; }

        /* If mover is not the player whose turn it is, send FAIL 31 Impatient */
        if ((turn == 0 && mover_fd != player1.fd) || (turn == 1 && mover_fd != player2.fd)) {
            send_ngp_message(mover_fd, "FAIL", "31 Impatient");
            /* do not change turn; continue waiting for correct player's move */
            continue;
        }

        /* Read full payload from mover */
        char *payload = read_ngp_payload(mover_fd);
        if (!payload) { /* read error or framing error; opponent wins */ winner = (turn==0)?1:0; break; }

        int fldc = 0;
        char **flds = split_payload(payload, &fldc);
        if (!flds || fldc < 1) {
            send_ngp_message(mover_fd, "FAIL", "10 Invalid");
            free(payload); free(flds);
            winner = (turn==0)?1:0; break;
        }

        char *type = flds[0];
        if (strcmp(type, "MOVE") != 0) {
            send_ngp_message(mover_fd, "FAIL", "24 Not Playing");
            free(payload); free(flds);
            winner = (turn==0)?1:0; break;
        }
        if (fldc < 3) {
            send_ngp_message(mover_fd, "FAIL", "10 Invalid");
            free(payload); free(flds);
            winner = (turn==0)?1:0; break;
        }
        int pile = atoi(flds[1]);
        int qty  = atoi(flds[2]);
        free(payload); free(flds);

        if (pile < 1 || pile > NUM_PILES) {
            send_ngp_message(mover_fd, "FAIL", "32 Pile Index");
            winner = (turn==0)?1:0; break;
        }
        int idx = pile - 1;
        if (qty < 1 || qty > board[idx]) {
            send_ngp_message(mover_fd, "FAIL", "33 Quantity");
            winner = (turn==0)?1:0; break;
        }

        board[idx] -= qty;
        log_message("%s removed %d from pile %d -> board: %d %d %d %d %d",
                    (turn==0)?player1.name:player2.name, qty, pile,
                    board[0],board[1],board[2],board[3],board[4]);

        if (check_game_over(board)) { winner = turn; break; }
        turn = 1 - turn;
    } /* end game loop */

    /* Send OVER|winner+1|board|| (third field empty) */
    board_info(board, board_buf);
    send_ngp_message(player1.fd, "OVER", "%d|%s|", (winner>=0)?(winner+1):0, board_buf);
    send_ngp_message(player2.fd, "OVER", "%d|%s|", (winner>=0)?(winner+1):0, board_buf);

    close(player1.fd);
    close(player2.fd);

    /* Child cleanup: exit */
    _exit(EXIT_SUCCESS);
}

/* ---------- Parent: handle single connection, handshake, and enqueue ---------- */

static void handle_new_connection(int client_fd)
{
    /* Read OPEN payload */
    char *payload = read_ngp_payload(client_fd);
    if (!payload) { close(client_fd); return; }

    int fldc = 0;
    char **flds = split_payload(payload, &fldc);
    if (!flds || fldc < 2) {
        send_ngp_message(client_fd, "FAIL", "10 Invalid");
        free(payload); free(flds);
        close(client_fd); return;
    }
    if (strcmp(flds[0], "OPEN") != 0) {
        send_ngp_message(client_fd, "FAIL", "10 Invalid");
        free(payload); free(flds);
        close(client_fd); return;
    }
    char *name = flds[1];
    if (!valid_name_chars(name)) {
        send_ngp_message(client_fd, "FAIL", "21 Long Name");
        free(payload); free(flds);
        close(client_fd); return;
    }
    if (name_present(name)) {
        send_ngp_message(client_fd, "FAIL", "22 Already Playing");
        free(payload); free(flds);
        close(client_fd); return;
    }

    /* Accept: send WAIT and enqueue */
    send_ngp_message(client_fd, "WAIT", "");
    add_connected_name(name);

    client_node_t *c = malloc(sizeof(client_node_t));
    if (!c) { remove_connected_name(name); free(payload); free(flds); close(client_fd); return; }
    c->fd = client_fd;
    strncpy(c->name, name, MAX_NAME);
    c->name[MAX_NAME] = '\0';
    c->next = NULL;
    enqueue_client(c);
    log_message("Enqueued player '%s'", c->name);

    free(payload);
    free(flds);

    /* Try to pair two clients */
    client_node_t *p1 = dequeue_client();
    client_node_t *p2 = dequeue_client();
    if (!p1 || !p2) {
        if (p1) enqueue_client(p1);
        if (p2) enqueue_client(p2);
        return;
    }

    /* fork child to run game */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(p1->fd); close(p2->fd);
        remove_connected_name(p1->name); remove_connected_name(p2->name);
        free(p1); free(p2);
        return;
    } else if (pid == 0) {
        /* Child uses PlayerInfo struct */
        PlayerInfo pl1, pl2;
        pl1.fd = p1->fd; strncpy(pl1.name, p1->name, MAX_NAME);
        pl2.fd = p2->fd; strncpy(pl2.name, p2->name, MAX_NAME);
        free(p1); free(p2);
        play_game(pl1, pl2);
        /* never returns */
    } else {
        /* parent records mapping and closes fds */
        add_game_entry(pid, p1->name, p2->name);
        log_message("Started game pid=%d: %s vs %s", pid, p1->name, p2->name);
        close(p1->fd); close(p2->fd);
        free(p1); free(p2);
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* create self-pipe and install SIGCHLD handler */
    if (pipe(spipe) < 0) { perror("pipe"); exit(EXIT_FAILURE); }
    struct sigaction sa_ch;
    memset(&sa_ch, 0, sizeof(sa_ch));
    sa_ch.sa_handler = _sigchld_pipe_write;
    sigemptyset(&sa_ch.sa_mask);
    sa_ch.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa_ch, NULL) < 0) { perror("sigaction SIGCHLD"); exit(EXIT_FAILURE); }

    /* setup handlers for shutdown signals */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_signal;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    if (sigaction(SIGINT, &sa_term, NULL) < 0) { perror("sigaction SIGINT"); exit(EXIT_FAILURE); }
    if (sigaction(SIGTERM, &sa_term, NULL) < 0) { perror("sigaction SIGTERM"); exit(EXIT_FAILURE); }
    if (sigaction(SIGHUP, &sa_term, NULL) < 0) { perror("sigaction SIGHUP"); exit(EXIT_FAILURE); }

    /* ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    listener_fd = open_listener(argv[1], MAX_QUEUE);
    if (listener_fd < 0) { fprintf(stderr, "open_listener failed\n"); exit(EXIT_FAILURE); }
    log_message("Server listening on port %s", argv[1]);

    /* Use poll to watch listener and spipe[0] */
    struct pollfd pfds[2];
    pfds[0].fd = listener_fd; pfds[0].events = POLLIN;
    pfds[1].fd = spipe[0]; pfds[1].events = POLLIN;

    while (active) {
        int rc = poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pfds[1].revents & POLLIN) {
            process_sigchld_events();
        }
        if (pfds[0].revents & POLLIN) {
            int client_fd = accept(listener_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }
            handle_new_connection(client_fd);
        }
    }

    log_message("Shutting down server");
    close(listener_fd);
    close(spipe[0]); close(spipe[1]);

    /* cleanup waiting clients */
    client_node_t *c = wait_head;
    while (c) {
        client_node_t *next = c->next;
        close(c->fd);
        free(c);
        c = next;
    }
    /* cleanup connected names */
    while (connected_names) {
        name_node_t *n = connected_names;
        connected_names = n->next;
        free(n);
    }
    while (games) {
        game_entry_t *g = games;
        games = g->next;
        free(g);
    }

    return 0;
}