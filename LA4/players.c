/*
 * players.c - Player-Parent (PP) and Player Processes for Snake Ludo
 * CS39002 Operating Systems Laboratory
 *
 * PP manages player processes and coordinates turns via signals.
 * Each player process handles dice rolling and movement.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BOARD_SIZE 101
#define MAX_PLAYERS 26

// Global variables for PP
int *shm_board = NULL;
int *shm_players = NULL;
int num_players = 0;
int pipe_fd = -1;  // Write end of pipe to CP
pid_t bp_pid = -1; // Board process PID
pid_t player_pids[MAX_PLAYERS];
int current_player = -1;
volatile sig_atomic_t move_requested = 0;
volatile sig_atomic_t should_exit = 0;

// Player symbols
const char player_symbols[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// For player process
volatile sig_atomic_t player_move_signal = 0;

// Signal handler for SIGUSR1 in PP - initiate move
void pp_sigusr1_handler(int sig) { move_requested = 1; }

// Signal handler for SIGUSR2 in PP - terminate
void pp_sigusr2_handler(int sig) { should_exit = 1; }

// Signal handler for SIGUSR1 in player - execute move
void player_sigusr1_handler(int sig) { player_move_signal = 1; }

// Roll dice with proper 6s handling
// Returns: total dice value, or 0 if three 6s (cancelled)
int roll_dice(int player_idx) {
  int total = 0;
  int rolls = 0;
  int die;
  int all_sixes = 1;

  printf("    %c throws: ", player_symbols[player_idx]);
  fflush(stdout);

  while (rolls < 3) {
    die = (rand() % 6) + 1;

    if (rolls > 0)
      printf("+ ");
    printf("%d ", die);
    fflush(stdout);

    total += die;
    rolls++;

    if (die != 6) {
      all_sixes = 0;
      break;
    }
  }

  // Three consecutive 6s cancel the move
  if (rolls == 3 && all_sixes) {
    printf("= %d (X) Three 6's! Move cancelled.\n", total);
    return 0;
  }

  printf("= %d\n", total);
  return total;
}

// Check if a cell is occupied by another player
int is_cell_occupied(int cell, int current_player_idx) {
  if (cell <= 0 || cell >= 100)
    return 0; // Home and finish are not "occupied"

  for (int i = 0; i < num_players; i++) {
    if (i != current_player_idx && shm_players[i] == cell) {
      return 1;
    }
  }
  return 0;
}

// Apply snakes and ladders, following chains
int apply_snakes_ladders(int pos, int player_idx) {
  int visited[BOARD_SIZE] = {0}; // Prevent infinite loops

  while (pos > 0 && pos < 100 && shm_board[pos] != 0 && !visited[pos]) {
    visited[pos] = 1;
    int modifier = shm_board[pos];
    int new_pos = pos + modifier;

    if (modifier > 0) {
      printf("    %c climbs ladder: %d -> %d\n", player_symbols[player_idx],
             pos, new_pos);
    } else {
      printf("    %c bitten by snake: %d -> %d\n", player_symbols[player_idx],
             pos, new_pos);
    }

    // Check if new position is occupied
    if (is_cell_occupied(new_pos, player_idx)) {
      printf("    But cell %d is occupied! Staying at %d\n", new_pos, pos);
      break;
    }

    pos = new_pos;
  }

  return pos;
}

// Player process main function
void player_process(int player_idx) {
  srand(time(NULL) ^ (getpid() << 16) ^ (player_idx * 12345));

  // Set up signal handler
  signal(SIGUSR1, player_sigusr1_handler);
  signal(SIGUSR2, SIG_DFL); // Default handler for SIGUSR2 (terminate)

  printf("+++ Player %c started (PID %d)\n", player_symbols[player_idx],
         getpid());
  fflush(stdout);

  while (1) {
    // Wait for signal to make a move
    pause();

    if (!player_move_signal)
      continue;
    player_move_signal = 0;

    int current_pos = shm_players[player_idx];

    // Check if already finished
    if (current_pos == 100) {
      // Signal BP to redraw (already at destination)
      kill(bp_pid, SIGUSR1);
      continue;
    }

    printf("\n>>> %c's turn (at cell %d)\n", player_symbols[player_idx],
           current_pos);
    fflush(stdout);

    // Roll dice
    int dice = roll_dice(player_idx);

    if (dice == 0) {
      // Move cancelled due to three 6s
      kill(bp_pid, SIGUSR1);
      continue;
    }

    int new_pos = current_pos + dice;

    // Check for exceeding 100
    if (new_pos > 100) {
      printf("    Move not allowed: %d + %d = %d > 100\n", current_pos, dice,
             new_pos);
      kill(bp_pid, SIGUSR1);
      continue;
    }

    // Check if target cell is occupied (before snakes/ladders)
    if (new_pos < 100 && is_cell_occupied(new_pos, player_idx)) {
      printf("    Move not allowed: cell %d is occupied\n", new_pos);
      kill(bp_pid, SIGUSR1);
      continue;
    }

    // Make the move
    printf("    %c moves: %d -> %d\n", player_symbols[player_idx], current_pos,
           new_pos);

    // Apply snakes and ladders (may chain)
    if (new_pos < 100) {
      new_pos = apply_snakes_ladders(new_pos, player_idx);
    }

    // Update position
    shm_players[player_idx] = new_pos;

    // Check for win
    if (new_pos == 100) {
      int rank = num_players - shm_players[num_players] + 1;
      printf("    *** %c reaches destination! Rank: %d ***\n",
             player_symbols[player_idx], rank);
      shm_players[num_players]--; // Decrement active count

      // Signal BP and exit
      kill(bp_pid, SIGUSR1);

      // Detach and exit
      shmdt(shm_board);
      shmdt(shm_players);
      exit(0);
    }

    // Signal BP to redraw
    kill(bp_pid, SIGUSR1);
  }
}

// Get next active player in round-robin
int get_next_player() {
  for (int i = 0; i < num_players; i++) {
    current_player = (current_player + 1) % num_players;
    // Check if player is still active (not at 100)
    if (shm_players[current_player] != 100) {
      return current_player;
    }
  }
  return -1; // No active players
}

// Player-Parent main function
void player_parent_process() {
  // Set up signal handlers for PP
  signal(SIGUSR1, pp_sigusr1_handler);
  signal(SIGUSR2, pp_sigusr2_handler);

  printf("+++ PP: Player-Parent started (PID %d)\n", getpid());
  printf("+++ PP: Board process PID: %d\n", bp_pid);
  printf("+++ PP: Creating %d player processes...\n\n", num_players);
  fflush(stdout);

  // Fork player processes
  for (int i = 0; i < num_players; i++) {
    player_pids[i] = fork();

    if (player_pids[i] < 0) {
      perror("fork (player)");
      exit(1);
    }

    if (player_pids[i] == 0) {
      // Child - player process
      player_process(i);
      exit(0); // Should never reach here
    }
  }

  // give children a moment to print their startup messages
  sleep(1);

  printf("+++ PP: All players ready\n");
  printf("-----------------------------------------------------\n\n");
  fflush(stdout);

  // Main loop
  while (!should_exit) {
    // Wait for signal
    pause();

    if (should_exit)
      break;

    if (move_requested) {
      move_requested = 0;

      // Check if any players remain
      if (shm_players[num_players] <= 0) {
        continue;
      }

      // Get next active player
      int next = get_next_player();
      if (next < 0) {
        continue;
      }

      // Signal the player to make their move
      kill(player_pids[next], SIGUSR1);
    }
  }

  // Termination - send SIGUSR2 to all player processes
  printf("\n+++ PP: Terminating player processes...\n");
  fflush(stdout);

  for (int i = 0; i < num_players; i++) {
    if (player_pids[i] > 0) {
      // Check if still alive
      if (kill(player_pids[i], 0) == 0) {
        kill(player_pids[i], SIGUSR2);
      }
    }
  }

  // Wait for all children
  for (int i = 0; i < num_players; i++) {
    if (player_pids[i] > 0) {
      waitpid(player_pids[i], NULL, 0);
      printf("+++ PP: Player %c terminated\n", player_symbols[i]);
      fflush(stdout);
      sleep(1); // Animation delay as per spec
    }
  }

  printf("+++ PP: All players terminated. Exiting.\n");
  fflush(stdout);
}

int main(int argc, char *argv[]) {
  if (argc < 6) {
    fprintf(stderr,
            "Usage: %s <shm_board_id> <shm_players_id> <num_players> <pipe_fd> "
            "<bp_pid>\n",
            argv[0]);
    return 1;
  }

  int shm_id_board = atoi(argv[1]);
  int shm_id_players = atoi(argv[2]);
  num_players = atoi(argv[3]);
  const char *fifo_path = argv[4];
  bp_pid = atoi(argv[5]);

  // Open FIFO for writing to CP
  pipe_fd = open(fifo_path, O_WRONLY);
  if (pipe_fd < 0) {
    perror("open fifo");
    return 1;
  }

  // Attach to shared memory segments
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

  printf("\n");
  printf("------------------------------------------------------\n");
  printf("|             SNAKE LUDO - Players Window            |\n");
  printf("------------------------------------------------------\n");
  printf("|  Players: %-3d                                      |\n",
         num_players);
  printf("------------------------------------------------------\n\n");
  fflush(stdout);

  // Send our PID to CP
  char pid_msg[64];
  sprintf(pid_msg, "PID:%d\n", getpid());
  write(pipe_fd, pid_msg, strlen(pid_msg));

  // Run player-parent process
  player_parent_process();

  // Cleanup
  shmdt(shm_board);
  shmdt(shm_players);

  return 0;
}
