#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#define MAX_LINE 1024      // maximum length for input command
#define MAX_ARGS 64        // maximum number of arguments for a command
#define HISTORY_COUNT 100  // maximum number of commands to store in history

// structure to store command history along with execution details
typedef struct {
    char *command;    // command string
    pid_t pid;        // process ID of the command
    time_t start_time;  // start time of the command
    long duration;    // duration of command execution (in seconds)
    int exit_status;  // exit status (0 for success, >0 for failure or signal termination)
} command_history;

// history storage
command_history history[HISTORY_COUNT];
int history_count = 0;  // counter for the number of commands in history

// function to add command details to history
void add_to_history(char *command, pid_t pid, time_t start_time, long duration, int exit_status) {
    command_history new_entry;
    new_entry.command = strdup(command);  // copy the command string
    new_entry.pid = pid;                  // store the process ID
    new_entry.start_time = start_time;    // store the start time
    new_entry.duration = duration;        // store the duration
    new_entry.exit_status = exit_status;  // store the exit status

    if (history_count < HISTORY_COUNT) {
        history[history_count] = new_entry; // add new entry to history
        history_count++;                     // increment history count
    } else {
        // if history is full, remove the oldest entry
        free(history[0].command);           // free the memory of the oldest command
        for (int i = 1; i < HISTORY_COUNT; i++) {
            history[i - 1] = history[i];    // shift history entries left
        }
        history[HISTORY_COUNT - 1] = new_entry; // add new entry at the end
    }
}

// function to execute a single command (without pipes)
void execute_command(char **args, char *input_command) {
    pid_t pid = fork();  // create a child process
    if (pid == 0) { // child process
        if (execvp(args[0], args) == -1) { // execute command
            perror("command execution failed"); // print error if execution fails
        }
        exit(EXIT_FAILURE); // exit child process if execution fails
    }
    else if (pid > 0) { // parent process
        time_t start_time = time(NULL);   // record the start time of the process
            
        int status;
        waitpid(pid, &status, 0);       // wait for the child process to complete
        time_t end_time = time(NULL);   // record the end time of the process

        long duration = end_time - start_time;  // calculate the duration of execution

        // add execution details to history
        add_to_history(input_command, pid, start_time, duration, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    } else {
        perror("fork failed"); // print error if fork fails
    }
}

// function to execute a command in the background
void execute_in_background(char *program, char **args) {
    pid_t pid = fork(); // create a child process

    if (pid == -1) {
        perror("fork failed"); // print error if fork fails
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        // child process: run in background
        int dev_null = open("/dev/null", O_RDWR); // open /dev/null for redirection
        if (dev_null == -1) {
            perror("failed to open /dev/null"); // print error if opening fails
            exit(EXIT_FAILURE);
        }
        dup2(dev_null, STDIN_FILENO);  // redirect stdin
        dup2(dev_null, STDOUT_FILENO); // redirect stdout
        dup2(dev_null, STDERR_FILENO); // redirect stderr
        close(dev_null);               // close the /dev/null descriptor

        // execute the command
        if (execvp(program, args) == -1) {
            perror("execvp failed"); // print error if execvp fails
            exit(EXIT_FAILURE);
        }
    } else {
        // parent process: do not wait for the child
        printf("command running in background with PID: %d\n", pid);

        // record the background command into history
        time_t start_time = time(NULL);  // capture the start time
        add_to_history(program, pid, start_time, 0, -1);  // duration and exit_status will be updated later
    }
}

// function to parse user input into arguments
void parse_command(char *input, char **args) {
    for (int i = 0; i < MAX_ARGS; i++) {
        args[i] = strsep(&input, " "); // separate input into arguments
        if (args[i] == NULL) break; // break if no more arguments
        if (strlen(args[i]) == 0) i--; // skip empty arguments
    }
}

// function to execute a command with multiple pipes
void execute_piped_command(char *input) {
    char *commands[MAX_ARGS]; // array to hold commands
    int num_pipes = 0;

    // parse the input for multiple commands separated by "|"
    commands[num_pipes] = strsep(&input, "|");
    while (commands[num_pipes] != NULL) {
        num_pipes++;
        commands[num_pipes] = strsep(&input, "|");
    }

    // array to hold file descriptors for pipes
    int pipe_fd[2 * num_pipes];
    for (int i = 0; i < num_pipes - 1; i++) {
        if (pipe(pipe_fd + i * 2) == -1) {
            perror("pipe failed"); // print error if pipe creation fails
            exit(EXIT_FAILURE);
        }
    }

    // loop to create child processes for each command
    for (int i = 0; i < num_pipes; i++) {
        char *args[MAX_ARGS]; // array to hold command arguments

        // store a copy of the original command for history before parsing
        char *original_command = strdup(commands[i]);  // copy the command for history

        // parse the command into arguments
        parse_command(commands[i], args);

        time_t start_time = time(NULL); // capture the start time
        pid_t pid = fork(); // create a child process
        if (pid == -1) {
            perror("fork failed"); // print error if fork fails
            exit(EXIT_FAILURE);
        }

        if (pid == 0) {  // child process
            // if it's not the first command, redirect stdin to the previous pipe's read end
            if (i > 0) {
                if (dup2(pipe_fd[(i - 1) * 2], STDIN_FILENO) == -1) {
                    perror("dup2 failed for input"); // print error if dup2 fails
                    exit(EXIT_FAILURE);
                }
            }

            // if it's not the last command, redirect stdout to the current pipe's write end
            if (i < num_pipes - 1) {
                if (dup2(pipe_fd[i * 2 + 1], STDOUT_FILENO) == -1) {
                    perror("dup2 failed for output"); // print error if dup2 fails
                    exit(EXIT_FAILURE);
                }
            }

            // close all pipe file descriptors
            for (int j = 0; j < 2 * (num_pipes - 1); j++) {
                close(pipe_fd[j]);
            }

            // execute the command
            if (execvp(args[0], args) == -1) {
                perror("command execution failed"); // print error if execution fails
                exit(EXIT_FAILURE);
            }
        }

        time_t end_time = time(NULL);  // capture the end time
        long duration = end_time - start_time;  // calculate the execution duration

        // use the original command for history
        add_to_history(original_command, pid, start_time, duration, 0);

        // free the copied original command
        free(original_command);
    }

    // parent process: close all pipe file descriptors
    for (int i = 0; i < 2 * (num_pipes - 1); i++) {
        close(pipe_fd[i]);
    }

    // wait for all child processes to complete
    for (int i = 0; i < num_pipes; i++) {
        wait(NULL);
    }
}

// function to print command history
void print_history() {
    for (int i = 0; i < history_count; i++) {
        printf("%d: command: %s | PID: %d | start: %ld | duration: %ld seconds\n",
               i + 1,
               history[i].command,
               history[i].pid,
               history[i].start_time,
               history[i].duration);
    }
}

int main() {
    char input[MAX_LINE]; // buffer for user input
    char *args[MAX_ARGS]; // array to hold command arguments

        do {
            printf("\033[32mSimpleShell> \033[0m"); // prompt for input
            if (fgets(input, MAX_LINE, stdin) == NULL) {
                perror("fgets failed"); // print error if fgets fails
                continue;
            }
            
            // remove newline character from input
            input[strlen(input) - 1] = '\0';

            // check for built-in commands
            if (strcmp(input, "history") == 0) {
                add_to_history("history", -1, -1, 0, 0); // add history command to history
                print_history(); // print command history
                continue;
            }

            // skip empty command
            if (strlen(input) == 0) {
                continue;
            }

            // check if it's a pipe command
            if (strchr(input, '|') != NULL) {
                execute_piped_command(input); // execute piped command
                continue;
            }

            // check if the command ends with '&' for background execution
            if (input[strlen(input) - 1] == '&') {
                input[strlen(input) - 1] = '\0';  // remove '&' from the input
                parse_command(input, args); // parse command
                execute_in_background(input, args); // execute in background
            }
            else {
                // parse the command
                parse_command(input, args);
                // execute the command
                execute_command(args, input);
            }
        } while (true);

    return 0; // exit program
}
 

