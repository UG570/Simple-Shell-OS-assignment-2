#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#define MAX_LINE 1024      // Maximum command length
#define MAX_ARGS 64        // Maximum arguments
#define HISTORY_COUNT 100  // Number of history commands to store

volatile sig_atomic_t foreground_running = 0;  // Track if a foreground process is running
pid_t foreground_pid = 0;                      // Track foreground process ID

// Structure to store command history along with execution details
typedef struct {
    char *command;    // Command string
    pid_t pid;        // Process ID of the command
    time_t start_time;  // Start time of the command
    long duration;    // Duration of command execution (in seconds)
    int exit_status;  // Exit status (0 for success, >0 for failure or signal termination)
} command_history;

// History storage
command_history history[HISTORY_COUNT];
int history_count = 0;

// Function to handle SIGINT (Ctrl-C)
void sigint_handler(int sig) {
    if (foreground_running && foreground_pid > 0) {
        // If a foreground process is running, kill it
        kill(foreground_pid, SIGINT);
        printf("\nForeground process %d terminated by Ctrl-C\n", foreground_pid);
    } else {
        // If no foreground process, exit the shell
        printf("\nExiting SimpleShell...\n");
        exit(0);
    }
}

// // Function to parse user input into arguments
// void parse_command(char *input, char **args) {
//     for (int i = 0; i < MAX_ARGS; i++) {
//         args[i] = strsep(&input, " ");
//         if (args[i] == NULL) break;
//         if (strlen(args[i]) == 0) i--; // Skip empty arguments
//     }
// }

// Function to add command details to history
void add_to_history(char *command, pid_t pid, time_t start_time, long duration, int exit_status) {
    command_history new_entry;
    new_entry.command = strdup(command);  // Copy the command string
    new_entry.pid = pid;                  // Store the process ID
    new_entry.start_time = start_time;    // Store the start time
    new_entry.duration = duration;        // Store the duration
    new_entry.exit_status = exit_status;  // Store the exit status

    if (history_count < HISTORY_COUNT) {
        history[history_count] = new_entry;
        history_count++;
    } else {
        // If history is full, remove the oldest entry
        free(history[0].command);
        for (int i = 1; i < HISTORY_COUNT; i++) {
            history[i - 1] = history[i];
        }
        history[HISTORY_COUNT - 1] = new_entry;
    }
}

// Function to execute a single command (without pipes)
void execute_command(char **args, bool background, char *input_command) {
    pid_t pid = fork();
    if (pid == 0) { // Child process
        if (execvp(args[0], args) == -1) {
            perror("Command execution failed");
        }
        exit(EXIT_FAILURE);
    } else if (pid > 0) { // Parent process
        time_t start_time = time(NULL);   // Record the start time of the process

        if (background) {
            // Background process: don't wait, just print the PID and continue
            printf("Background process started with PID: %d\n", pid);
            add_to_history(input_command, pid, start_time, 0, -1);  // Add to history without waiting for completion
        } else {
            // Foreground process: wait for it to complete
            foreground_pid = pid;           // Set the foreground PID
            foreground_running = 1;         // Set flag to indicate foreground process is running
            
            int status;
            waitpid(pid, &status, 0);       // Wait for the child process to complete
            time_t end_time = time(NULL);   // Record the end time of the process

            long duration = end_time - start_time;  // Calculate the duration of execution
            foreground_running = 0;         // Reset flag when process finishes

            if (WIFSIGNALED(status)) {
                printf("PID: %d terminated by signal %d\n", pid, WTERMSIG(status));
            } else {
                printf("PID: %d, Start: %ld, Duration: %ld seconds\n", pid, start_time, duration);
            }

            foreground_pid = 0;  // Reset foreground PID after process completion

            // Add execution details to history
            add_to_history(input_command, pid, start_time, duration, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    } else {
        perror("Fork failed");
    }
}

// Function to parse user input into arguments
void parse_command(char *input, char **args) {
    for (int i = 0; i < MAX_ARGS; i++) {
        args[i] = strsep(&input, " ");
        if (args[i] == NULL) break;
        if (strlen(args[i]) == 0) i--; // Skip empty arguments
    }
}

// Function to execute a command with multiple pipes
void execute_piped_command(char *input) {
    // Split the input by pipe symbol "|"
    char *commands[MAX_ARGS];
    int num_pipes = 0;

    // Parse the input for multiple commands separated by "|"
    commands[num_pipes] = strsep(&input, "|");
    while (commands[num_pipes] != NULL) {
        num_pipes++;
        commands[num_pipes] = strsep(&input, "|");
    }

    // Array to hold file descriptors for pipes
    int pipe_fd[2 * num_pipes];
    for (int i = 0; i < num_pipes - 1; i++) {
        if (pipe(pipe_fd + i * 2) == -1) {
            perror("Pipe failed");
            exit(EXIT_FAILURE);
        }
    }

    // Loop to create child processes for each command
    for (int i = 0; i < num_pipes; i++) {
        char *args[MAX_ARGS];
        parse_command(commands[i], args);

        pid_t pid = fork();
        if (pid == -1) {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        }

        if (pid == 0) {  // Child process
            // If it's not the first command, redirect stdin to the previous pipe's read end
            if (i > 0) {
                if (dup2(pipe_fd[(i - 1) * 2], STDIN_FILENO) == -1) {
                    perror("dup2 failed for input");
                    exit(EXIT_FAILURE);
                }
            }

            // If it's not the last command, redirect stdout to the current pipe's write end
            if (i < num_pipes - 1) {
                if (dup2(pipe_fd[i * 2 + 1], STDOUT_FILENO) == -1) {
                    perror("dup2 failed for output");
                    exit(EXIT_FAILURE);
                }
            }

            // Close all pipe file descriptors
            for (int j = 0; j < 2 * (num_pipes - 1); j++) {
                close(pipe_fd[j]);
            }

            // Execute the command
            if (execvp(args[0], args) == -1) {
                perror("Command execution failed");
                exit(EXIT_FAILURE);
            }
        }
    }

    // Parent process: close all pipe file descriptors
    for (int i = 0; i < 2 * (num_pipes - 1); i++) {
        close(pipe_fd[i]);
    }

    // Wait for all child processes to complete and calculate the execution duration
    time_t start_time = time(NULL);
    for (int i = 0; i < num_pipes; i++) {
        wait(NULL);
    }
    time_t end_time = time(NULL); // Record end time

    long duration = end_time - start_time;  // Calculate the duration of execution

    // Add execution details to history
    add_to_history(input, 0, start_time, duration, 0); // PID can be set to 0 for piped commands
}

int main() {
    char input[MAX_LINE];
    char *args[MAX_ARGS];

    // Set up the SIGINT handler
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    while (true) {
        printf("SimpleShell> ");
        if (fgets(input, MAX_LINE, stdin) == NULL) {
            perror("fgets failed");
            continue;
        }
        
        // Remove newline character from input
        input[strlen(input) - 1] = '\0';

        // Check for built-in commands
        if (strcmp(input, "history") == 0) {
            print_history();
            continue;
        }

        // Skip empty command
        if (strlen(input) == 0) {
            continue;
        }

        // Check if it's a pipe command
        if (strchr(input, '|') != NULL) {
            execute_piped_command(input);
            continue;
        }

        bool background = false;

        // Check if the command ends with '&' for background execution
        if (input[strlen(input) - 1] == '&') {
            background = true;
            input[strlen(input) - 1] = '\0';  // Remove '&' from the input
        }

        // Parse the command
        parse_command(input, args);

        // Execute the command
        execute_command(args, background, input);
    }

    return 0;
}


// Function to print command history
void print_history() {
    for (int i = 0; i < history_count; i++) {
        printf("%d: Command: %s | PID: %d | Start: %ld | Duration: %ld seconds | Status: %d\n",
               i + 1,
               history[i].command,
               history[i].pid,
               history[i].start_time,
               history[i].duration,
               history[i].exit_status);
    }
}