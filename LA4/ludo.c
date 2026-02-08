/*
 * ludo.c - Coordinator Process (CP) for Snake Ludo
 * CS39002 Operating Systems Laboratory
 *
 * This process creates shared memory, spawns board and player processes,
 * and coordinates the game through signals and pipes.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIFO_NAME "/tmp/ludo_fifo"

#define MAX_PLAYERS 26
#define BOARD_SIZE 101 // 0-100, index 0 unused
#define SHM_KEY_BOARD 0x1234
#define SHM_KEY_PLAYERS 0x5678

// Global variables for cleanup
int shm_id_board = -1;
int shm_id_players = -1;
int *shm_board = NULL;
int *shm_players = NULL;
pid_t xbp_pid = -1; // XBP (xterm for board)
pid_t xpp_pid = -1; // XPP (xterm for players)
pid_t bp_pid = -1;  // BP (board process, child of XBP)
pid_t pp_pid = -1;  // PP (player-parent, child of XPP)
int pipe_fd = -1;   // Pipe for BP/PP -> CP communication (Read end)
int num_players = 0;
volatile sig_atomic_t game_over = 0;

// Signal handler for SIGINT
void sigint_handler(int sig) { game_over = 1; }

// Check if xterm is available
int check_xterm() {
  if (system("which xterm > /dev/null 2>&1") != 0) {
    fprintf(stderr, "Error: xterm is not installed or not in PATH.\n");
    fprintf(stderr, "Please install it (e.g., sudo apt install xterm)\n");
    return -1;
  }
  return 0;
}

// Read board configuration from ludo.txt
int read_board_from_file(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (fp == NULL) {
    perror("fopen (ludo.txt)");
    return -1;
  }

  // Clear board
  for (int i = 0; i < BOARD_SIZE; i++) {
    shm_board[i] = 0;
  }

  char type;
  int from, to;

  while (fscanf(fp, " %c", &type) == 1) {
    if (type == 'E') {
      break; // End of board
    }

    if (fscanf(fp, "%d %d", &from, &to) != 2) {
      fprintf(stderr, "Error reading board file\n");
      fclose(fp);
      return -1;
    }

    if (type == 'L') {
      // Ladder: store (top - bottom)
      shm_board[from] = to - from;
      printf("  Ladder: %d -> %d\n", from, to);
    } else if (type == 'S') {
      // Snake: store (tail - mouth)
      shm_board[from] = to - from;
      printf("  Snake: %d -> %d\n", from, to);
    }
  }

  fclose(fp);
  return 0;
}

// Create shared memory segments
int create_shared_memory() {
  // Create board shared memory
  shm_id_board = shmget(SHM_KEY_BOARD, BOARD_SIZE * sizeof(int),
                        IPC_CREAT | IPC_EXCL | 0666);
  if (shm_id_board < 0) {
    perror("shmget (board)");
    return -1;
  }

  shm_board = (int *)shmat(shm_id_board, NULL, 0);
  if (shm_board == (int *)-1) {
    perror("shmat (board)");
    return -1;
  }

  // Create players shared memory (num_players + 1 for active count)
  shm_id_players = shmget(SHM_KEY_PLAYERS, (MAX_PLAYERS + 1) * sizeof(int),
                          IPC_CREAT | IPC_EXCL | 0666);
  if (shm_id_players < 0) {
    perror("shmget (players)");
    return -1;
  }

  shm_players = (int *)shmat(shm_id_players, NULL, 0);
  if (shm_players == (int *)-1) {
    perror("shmat (players)");
    return -1;
  }

  // Initialize player positions to 0 (home)
  for (int i = 0; i < MAX_PLAYERS; i++) {
    shm_players[i] = 0;
  }
  shm_players[num_players] = num_players; // Active player count

  return 0;
}

// Cleanup shared memory and processes
void cleanup() {
  printf("\n+++ CP: Cleaning up...\n");

  // Send SIGUSR2 to PP to terminate players
  if (pp_pid > 0) {
    printf("+++ CP: Sending SIGUSR2 to PP (PID %d)\n", pp_pid);
    kill(pp_pid, SIGUSR2);
  }

  // Wait for XPP to terminate
  if (xpp_pid > 0) {
    printf("+++ CP: Waiting for XPP to terminate...\n");
    waitpid(xpp_pid, NULL, 0);
    printf("+++ CP: XPP terminated\n");
  }

  // Send SIGUSR2 to BP
  if (bp_pid > 0) {
    printf("+++ CP: Sending SIGUSR2 to BP (PID %d)\n", bp_pid);
    kill(bp_pid, SIGUSR2);
  }

  // Wait for XBP to terminate
  if (xbp_pid > 0) {
    printf("+++ CP: Waiting for XBP to terminate...\n");
    waitpid(xbp_pid, NULL, 0);
    printf("+++ CP: XBP terminated\n");
  }

  // Close pipe and remove FIFO
  if (pipe_fd != -1)
    close(pipe_fd);
  unlink(FIFO_NAME);

  // Detach shared memory
  if (shm_board != NULL && shm_board != (int *)-1) {
    shmdt(shm_board);
  }
  if (shm_players != NULL && shm_players != (int *)-1) {
    shmdt(shm_players);
  }

  // Remove shared memory segments
  if (shm_id_board >= 0) {
    shmctl(shm_id_board, IPC_RMID, NULL);
    printf("+++ CP: Removed board shared memory\n");
  }
  if (shm_id_players >= 0) {
    shmctl(shm_id_players, IPC_RMID, NULL);
    printf("+++ CP: Removed players shared memory\n");
  }

  printf("+++ CP: Cleanup complete. Goodbye!\n");
}

// Spawn board process via xterm
pid_t spawn_board_xterm() {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork (xterm board)");
    return -1;
  }

  if (pid == 0) {
    // Child - exec xterm with board process
    char shm_board_str[32], shm_players_str[32], num_players_str[16];
    sprintf(shm_board_str, "%d", shm_id_board);
    sprintf(shm_players_str, "%d", shm_id_players);
    sprintf(num_players_str, "%d", num_players);

    execlp("xterm", "xterm", "-T", "Board", "-fn", "fixed", "-geometry",
           "150x24+50+50", "-bg", "#003300", "-fg", "white", "-e", "./board",
           shm_board_str, shm_players_str, num_players_str, FIFO_NAME,
           (char *)NULL);
    perror("execlp (xterm board)");
    exit(1);
  }

  return pid;
}

// Spawn players process via xterm
pid_t spawn_players_xterm() {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork (xterm players)");
    return -1;
  }

  if (pid == 0) {
    // Child - exec xterm with players process
    char shm_board_str[32], shm_players_str[32], num_players_str[16];
    char bp_pid_str[16];
    sprintf(shm_board_str, "%d", shm_id_board);
    sprintf(shm_players_str, "%d", shm_id_players);
    sprintf(num_players_str, "%d", num_players);
    sprintf(bp_pid_str, "%d", bp_pid);

    execlp("xterm", "xterm", "-T", "Players", "-fn", "fixed", "-geometry",
           "100x24+400+50", "-bg", "#000033", "-fg", "white", "-e", "./players",
           shm_board_str, shm_players_str, num_players_str, FIFO_NAME,
           bp_pid_str, (char *)NULL);
    perror("execlp (xterm players)");
    exit(1);
  }

  return pid;
}

// Helper to read a line from pipe (byte-by-byte for safety)
int read_line_from_fifo(int fd, char *buffer, int max_len) {
  int i = 0;
  char c;
  while (i < max_len - 1) {
    int n = read(fd, &c, 1);
    if (n > 0) {
      if (c == '\n')
        break;
      buffer[i++] = c;
    } else if (n == 0) {
      // EOF
      return (i > 0) ? i : -1;
    } else {
      if (errno == EINTR)
        continue;
      return -1;
    }
  }
  buffer[i] = '\0';
  return i;
}

// Wait for acknowledgment from BP via pipe
void wait_for_ack() {
  char buffer[64];
  // Blocking read until a line is available
  if (read_line_from_fifo(pipe_fd, buffer, sizeof(buffer)) > 0) {
    if (strncmp(buffer, "ACK", 3) != 0) {
      // Note: In a robust app we might handle unexpected msg,
      // but here we just proceed.
      // fprintf(stderr, "CP: Warning, expected ACK, got '%s'\n", buffer);
    }
  }
}

// Read PID from pipe
pid_t read_pid_from_pipe() {
  char buffer[64];
  if (read_line_from_fifo(pipe_fd, buffer, sizeof(buffer)) > 0) {
    // Parse PID: format is "PID:12345"
    if (strncmp(buffer, "PID:", 4) == 0) {
      return (pid_t)atoi(buffer + 4);
    }
  }
  return -1;
}

void print_usage(char *prog_name) {
  printf("Usage: %s <num_players>\n", prog_name);
  printf("  num_players: 2-%d\n", MAX_PLAYERS);
  printf("\nCommands during interactive mode:\n");
  printf("  next          - Execute next player's move\n");
  printf("  delay <ms>    - Set delay for autoplay (default: 1000)\n");
  printf("  autoplay      - Switch to autoplay mode\n");
  printf("  quit          - End the game\n");
}

int main(int argc, char *argv[]) {
  int delay_ms = 1000;
  int autoplay = 0;

  // Parse arguments
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  num_players = atoi(argv[1]);
  if (num_players < 2 || num_players > MAX_PLAYERS) {
    fprintf(stderr, "Error: num_players must be 2-%d\n", MAX_PLAYERS);
    return 1;
  }

  printf("\n");
  printf("------------------------------------------------------\n");
  printf("|          SNAKE LUDO - Coordinator Process          |\n");
  printf("------------------------------------------------------\n");
  printf("|  Players: %-3d                                      |\n",
         num_players);
  printf("------------------------------------------------------\n\n");

  // Set up signal handlers
  signal(SIGINT, sigint_handler);
  signal(SIGPIPE, SIG_IGN);

  // Check for xterm
  if (check_xterm() < 0)
    return 1;

  // Create named pipe for BP/PP -> CP communication
  unlink(FIFO_NAME); // Remove if exists
  if (mkfifo(FIFO_NAME, 0666) < 0) {
    perror("mkfifo");
    return 1;
  }
  printf("+++ CP: Created FIFO %s\n", FIFO_NAME);

  // Create shared memory
  printf("+++ CP: Creating shared memory segments...\n");
  if (create_shared_memory() < 0) {
    fprintf(stderr, "Failed to create shared memory\n");
    return 1;
  }
  printf("+++ CP: Shared memory created (MB=%d, MP=%d)\n", shm_id_board,
         shm_id_players);

  // Read board from ludo.txt
  printf("+++ CP: Reading board from ludo.txt...\n");
  if (read_board_from_file("ludo.txt") < 0) {
    fprintf(stderr, "Failed to read board file\n");
    cleanup();
    return 1;
  }
  printf("+++ CP: Board initialized\n");

  // Spawn board xterm (XBP -> BP)
  printf("+++ CP: Spawning board window...\n");
  xbp_pid = spawn_board_xterm();
  if (xbp_pid < 0) {
    cleanup();
    return 1;
  }
  printf("+++ CP: XBP spawned (PID %d)\n", xbp_pid);

  // Open FIFO for reading (blocks until writer connects)
  printf("+++ CP: Waiting for Board process to connect...\n");
  pipe_fd = open(FIFO_NAME, O_RDONLY);
  if (pipe_fd < 0) {
    perror("open fifo");
    cleanup();
    return 1;
  }

  // Read BP's PID from pipe
  bp_pid = read_pid_from_pipe();
  printf("+++ CP: BP started (PID %d)\n", bp_pid);

  // Spawn players xterm (XPP -> PP -> A, B, C, ...)
  printf("+++ CP: Spawning players window...\n");
  xpp_pid = spawn_players_xterm();
  if (xpp_pid < 0) {
    cleanup();
    return 1;
  }
  printf("+++ CP: XPP spawned (PID %d)\n", xpp_pid);

  // Read PP's PID from pipe
  pp_pid = read_pid_from_pipe();
  printf("+++ CP: PP started (PID %d)\n", pp_pid);

  // Wait for initial board print acknowledgment
  printf("+++ CP: Waiting for initial board...\n");
  wait_for_ack();
  printf("+++ CP: Game ready!\n\n");

  printf("Commands: next, delay <ms>, autoplay, quit\n");
  printf("-----------------------------------------------------\n\n");

  // Game loop
  char input[128];
  struct timespec ts;

  while (!game_over && shm_players[num_players] > 0) {
    if (autoplay) {
      // Autoplay mode: automatic moves with delay
      ts.tv_sec = delay_ms / 1000;
      ts.tv_nsec = (delay_ms % 1000) * 1000000L;
      nanosleep(&ts, NULL);

      // Check if game ended during sleep
      if (game_over || shm_players[num_players] <= 0)
        break;

      // Signal PP to make next move
      kill(pp_pid, SIGUSR1);

      // Wait for acknowledgment from BP
      wait_for_ack();
    } else {
      // Interactive mode: wait for user command
      printf("+++ CP: Enter command: ");
      fflush(stdout);

      if (fgets(input, sizeof(input), stdin) == NULL) {
        break;
      }

      // Remove newline
      input[strcspn(input, "\n")] = 0;

      if (strcmp(input, "quit") == 0) {
        printf("+++ CP: User requested quit\n");
        game_over = 1;
        break;
      } else if (strcmp(input, "next") == 0) {
        // Signal PP to make next move
        kill(pp_pid, SIGUSR1);
        // Wait for acknowledgment from BP
        wait_for_ack();
      } else if (strncmp(input, "delay ", 6) == 0) {
        delay_ms = atoi(input + 6);
        if (delay_ms < 0)
          delay_ms = 0;
        printf("+++ CP: Delay set to %d ms\n", delay_ms);
      } else if (strcmp(input, "autoplay") == 0) {
        autoplay = 1;
        printf("+++ CP: Switching to autoplay mode (delay: %d ms)\n", delay_ms);
      } else if (strlen(input) > 0) {
        printf("+++ CP: Unknown command '%s'\n", input);
      }
    }
  }

  // Game over
  if (shm_players[num_players] <= 0) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║              ALL PLAYERS HAVE FINISHED!              ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
  }

  printf("+++ CP: Press ENTER to exit...");
  fflush(stdout);
  getchar();

  // Cleanup
  cleanup();

  return 0;
}