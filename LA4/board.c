/*
 * board.c - Board Process (BP) for Snake Ludo
 * CS39002 Operating Systems Laboratory
 *
 * This process displays the game board, updating on SIGUSR1 signals.
 * Sends acknowledgment to CP after each board print via pipe.
 * Terminates on SIGUSR2 from the coordinator.
 *
 * Author: Ashutosh Sharma
 * Roll: 23CS10005
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#define BOARD_SIZE 101
#define MAX_PLAYERS 26

// Global variables
int *shm_board = NULL;
int *shm_players = NULL;
int num_players = 0;
int pipe_fd = -1; // write end of pipe to CP
volatile sig_atomic_t should_redraw = 1;
volatile sig_atomic_t should_exit = 0;

const char player_symbols[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

void sigusr1_handler(int sig) { should_redraw = 1; }
void sigusr2_handler(int sig) { should_exit = 1; }

// get cell number for board position (zigzag pattern)
// row 0 is top (cells 91-100), row 9 is bottom (cells 1-10)
int get_display_cell(int row, int col) {
  int base_row = 9 - row; // flip: row 0 is top

  if (base_row % 2 == 0) {
    // even rows (0, 2, 4, 6, 8) go left to right
    return base_row * 10 + col + 1;
  } else {
    // odd rows (1, 3, 5, 7, 9) go right to left
    return base_row * 10 + (10 - col);
  }
}

int get_players_on_cell(int cell) {
  int mask = 0;
  for (int i = 0; i < num_players; i++) {
    if (shm_players[i] == cell && cell > 0 && cell < 100) {
      mask |= (1 << i);
    }
  }
  return mask;
}

void send_ack() {
  const char *ack = "ACK\n";
  write(pipe_fd, ack, strlen(ack));
}

void print_board() {
  printf("\033[2J\033[H");

  // print header with finished players
  printf("+");
  for (int i = 0; i < 72; i++)
    printf("-");
  printf("+\n");

  printf("|  Finished: ");
  int count = 0;
  for (int i = 0; i < num_players; i++) {
    if (shm_players[i] == 100) {
      if (count > 0)
        printf(", ");
      printf("%c", player_symbols[i]);
      count++;
    }
  }
  if (count == 0)
    printf("(none)");
  for (int i = 0; i < 72 - 12 - (count > 0 ? count * 3 : 6); i++)
    printf(" ");
  printf("|\n");

  printf("+");
  for (int i = 0; i < 72; i++)
    printf("-");
  printf("+\n");

  for (int row = 0; row < 10; row++) {
    printf("| ");
    for (int col = 0; col < 10; col++) {
      int cell = get_display_cell(row, col);
      int players = get_players_on_cell(cell);
      int cell_val = shm_board[cell];

      if (players) {
        printf("\033[1;33m"); // yellow/bold
        for (int p = 0; p < num_players; p++) {
          if (players & (1 << p)) {
            printf("%c", player_symbols[p]);
            break; // show first player only
          }
        }
        printf("\033[0m");
        printf("%-5d ", cell);
      } else if (cell_val > 0) {
        // ladder - green
        printf("\033[32mL%-5d\033[0m ", cell);
      } else if (cell_val < 0) {
        // snake - red
        printf("\033[31mS%-5d\033[0m ", cell);
      } else {
        // empty cell
        printf("%-6d ", cell);
      }
    }
    printf("|\n");
  }

  printf("+");
  for (int i = 0; i < 72; i++)
    printf("-");
  printf("+\n");

  printf("|  Home: ");
  count = 0;
  for (int i = 0; i < num_players; i++) {
    if (shm_players[i] == 0) {
      if (count > 0)
        printf(", ");
      printf("%c", player_symbols[i]);
      count++;
    }
  }
  if (count == 0)
    printf("(none)");
  for (int i = 0; i < 72 - 8 - (count > 0 ? count * 3 : 6); i++)
    printf(" ");
  printf("|\n");

  // print active count
  printf("|  Active players: %d / %d", shm_players[num_players], num_players);
  for (int i = 0; i < 72 - 22; i++)
    printf(" ");
  printf("|\n");

  printf("+");
  for (int i = 0; i < 72; i++)
    printf("-");
  printf("+\n");

  // legend
  printf("\n  \033[32mL\033[0m = Ladder   \033[31mS\033[0m = Snake   "
         "\033[1;33mX\033[0m = Player X at cell\n");

  fflush(stdout);
}

int main(int argc, char *argv[]) {
  if (argc < 5) {
    fprintf(
        stderr,
        "Usage: %s <shm_board_id> <shm_players_id> <num_players> <pipe_fd>\n",
        argv[0]);
    return 1;
  }

  int shm_id_board = atoi(argv[1]);
  int shm_id_players = atoi(argv[2]);
  num_players = atoi(argv[3]);
  const char *fifo_path = argv[4];

  // open fifo for writing to CP
  pipe_fd = open(fifo_path, O_WRONLY);
  if (pipe_fd < 0) {
    perror("open fifo");
    return 1;
  }

  shm_board = (int *)shmat(shm_id_board, NULL, SHM_RDONLY);
  if (shm_board == (int *)-1) {
    perror("shmat (board)");
    return 1;
  }

  shm_players = (int *)shmat(shm_id_players, NULL, 0);
  if (shm_players == (int *)-1) {
    perror("shmat (players)");
    return 1;
  }

  signal(SIGUSR1, sigusr1_handler);
  signal(SIGUSR2, sigusr2_handler);

  char pid_msg[64];
  sprintf(pid_msg, "PID:%d\n", getpid());
  write(pipe_fd, pid_msg, strlen(pid_msg));

  sleep(1);

  print_board();
  send_ack();
  should_redraw = 0;

  while (!should_exit) {
    if (should_redraw) {
      should_redraw = 0;
      print_board();
      send_ack();
    }

    if (!should_exit) {
      pause();
    }
  }

  printf("\n+++ BP: Board process terminating...\n");
  shmdt(shm_board);
  shmdt(shm_players);

  return 0;
}
