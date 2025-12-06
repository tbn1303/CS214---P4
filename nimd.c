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
    vfprintf(stdout, format, ap); // Print formatted message to stdout
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

// Send message to client
int send_ngp_message(int fd, const char *type, const char *format, ...) {
    char buffer[256];
    char message[128];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (n < 0) {
        return -1; // Encoding error
    }
    return send_message(fd, buffer);
}

// Game information structure
typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} PlayerInfo;

//Game info message
void board_info(int *board, char *buf) {
    sprintf(buf, "BOARD %d %d %d %d %d", board[0], board[1], board[2], board[3], board[4]);
}

// Check for game over condition
int check_game_over(int *board) {
    for (int i = 0; i < NUM_PILES; i++) {
        if (board[i] > 0) {

            return 0; // Game is not over
        }
    }
    return 1; // Game is over
}

// Gameplay function
void play_game(PlayerInfo player1, PlayerInfo player2) {
    int board[NUM_PILES] = {3, 5, 7, 9, 11}; // Initial board setup
    int turn = 0; // 0 for player1's turn, 1 for player2's turn
    int winner = -1; // -1 for no winner, 0 for player1, 1 for player2
    char board_buf[128];

    // Send player names messages
    send_ngp_message(player1.fd, "NAME", "1|%s", player1.name);
    send_ngp_message(player2.fd, "NAME", "2|%s", player2.name);

    while (!is_game_over()) {
        PlayerInfo current_player = (turn == 0) ? player1 : player2;
        PlayerInfo other_player = (turn == 0) ? player2 : player1;

        // Send board state to both players
        board_info(board, board_buf);
        send_ngp_message(current_player.fd, "PLAY", "%d|%s", turn, board_buf);
        send_ngp_message(other_player.fd, "PLAY", "%d|%s", turn, board_buf);

        // Receive move from current player
        char move_buf[128];
        int n = receive_message(current_player.fd, move_buf, sizeof(move_buf) - 1, 0);
        if (n <= 0) {
            winner = (turn == 0) ? 1 : 0; // Other player wins if current player disconnects
            break;
        }

        move_buf[n] = '\0';

        int pile, stones;
        if (sscanf(move_buf, "0|%*2[0-9]|MOVE|%d|%d|", &pile, &stones) != 2 || pile < 1 || pile > NUM_PILES || stones < 1 || stones > board[pile - 1]) {
            // Invalid move
            send_ngp_message(current_player.fd, "FAIL", "32 Pile Index or 33 Quantity");
            continue; // Retry the move
        }

        //update board state
        board[pile - 1] -= stones;
        // Check for game over condition
        if (check_game_over(board)) {
            winner = turn; // Current player wins
            break;
        }

        turn = (turn == 0) ? 1 : 0; // Switch turns
    }

    // Send game over message to both players
    board_info(board, board_buf);
    send_ngp_message(player1.fd, "OVER", "%d|%s|", winner, board_buf);
    send_ngp_message(player2.fd, "OVER", "%d|%s|", winner, board_buf);

    close(player1.fd);
    close(player2.fd);
}
    