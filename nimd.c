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
#define BUF_SIZE 256

volatile sig_atomic_t active = 1;

typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} PlayerInfo;

// --------------------- Logging ---------------------
void log_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    va_end(ap);
}

// --------------------- Signals ---------------------
void handle_signal(int sig) { active = 0; }

void reap_children(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    struct sigaction sa_chld;
    sa_chld.sa_handler = reap_children;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);
}

// --------------------- Socket I/O ---------------------
int send_message(int sock, const char *msg) {
    int total = (int)strlen(msg);
    int sent = 0;
    while (sent < total) {
        int n = write(sock, msg + sent, total - sent);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return 0;
}

int receive_message(int sock, char *buf, int maxlen) {
    int n = read(sock, buf, maxlen);
    if (n < 0) return -1;
    return n;
}

// --------------------- NGP Messaging ---------------------
int send_ngp_message(int fd, const char *type, const char *fmt, ...) {
    char buffer[BUF_SIZE], msg[BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    snprintf(msg, sizeof(msg), "0|%02zu|%s|%s|", strlen(buffer)+strlen(type)+2, type, buffer);
    return send_message(fd, msg);
}

// --------------------- Game ---------------------
void board_info(int board[NUM_PILES], char *buf) {
    sprintf(buf, "%d %d %d %d %d", board[0], board[1], board[2], board[3], board[4]);
}

int check_game_over(int board[NUM_PILES]) {
    for (int i = 0; i < NUM_PILES; i++) {
        if (board[i] > 0) return 0;
    }
    return 1;
}

void play_game(PlayerInfo p1, PlayerInfo p2) {
    int board[NUM_PILES] = {3, 5, 7, 9, 11};
    int turn = 0, winner = -1;
    char board_buf[128];

    send_ngp_message(p1.fd, "NAME", "1|%s", p2.name);
    send_ngp_message(p2.fd, "NAME", "2|%s", p1.name);

    while (1) {
        PlayerInfo cur = (turn == 0) ? p1 : p2;
        PlayerInfo other = (turn == 0) ? p2 : p1;

        board_info(board, board_buf);
        send_ngp_message(cur.fd, "PLAY", "%d|%s", turn + 1, board_buf);
        send_ngp_message(other.fd, "PLAY", "%d|%s", turn + 1, board_buf);

        char move_buf[128];
        int n = receive_message(cur.fd, move_buf, sizeof(move_buf)-1);
        if (n <= 0) {
            winner = (turn == 0) ? 2 : 1; // opponent wins
            break;
        }
        move_buf[n] = '\0';

        int pile, stones;
        if (sscanf(move_buf, "0|%*2[0-9]|MOVE|%d|%d|", &pile, &stones) != 2 ||
            pile < 1 || pile > NUM_PILES || stones < 1 || stones > board[pile-1]) {
            send_ngp_message(cur.fd, "FAIL", "32 Pile Index or 33 Quantity");
            continue;
        }

        board[pile-1] -= stones;
        if (check_game_over(board)) {
            winner = turn + 1;
            break;
        }
        turn = (turn == 0) ? 1 : 0;
    }

    board_info(board, board_buf);
    send_ngp_message(p1.fd, "OVER", "%d|%s|", winner, board_buf);
    send_ngp_message(p2.fd, "OVER", "%d|%s|", winner, board_buf);

    close(p1.fd);
    close(p2.fd);
}

// --------------------- Main ---------------------
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    setup_signals();
    int server_fd = open_listener(argv[1], MAX_QUEUE);
    if (server_fd < 0) {
        perror("open_listener");
        exit(EXIT_FAILURE);
    }

    PlayerInfo players[2];
    int player_count = 0;

    while (active) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char type_buf[128];
        int n = receive_message(client_fd, type_buf, sizeof(type_buf)-1);
        if (n <= 0) { close(client_fd); continue; }
        type_buf[n] = '\0';

        char name_buf[MAX_NAME + 1];
        if (sscanf(type_buf, "0|%*2[0-9]|OPEN|%72[^\n]|", name_buf) != 1) {
            send_ngp_message(client_fd, "FAIL", "10 Invalid");
            close(client_fd);
            continue;
        }

        strncpy(players[player_count].name, name_buf, MAX_NAME);
        players[player_count].fd = client_fd;
        send_ngp_message(client_fd, "WAIT", "");
        player_count++;

        if (player_count == 2) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                close(players[0].fd);
                close(players[1].fd);
            } else if (pid == 0) {
                close(server_fd);
                play_game(players[0], players[1]);
                exit(EXIT_SUCCESS);
            } else {
                close(players[0].fd);
                close(players[1].fd);
            }
            player_count = 0;
        }
    }

    log_message("Shutting down server");
    close(server_fd);
    return 0;
}