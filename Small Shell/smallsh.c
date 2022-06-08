#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_ARGS 512
#define MAX_LINE 2048
#define MAX_DIR_LEN 256
#define MAX_FN_LEN 256

/*
RESOURCES

Multiple topics: 
- 3.1 Processes: Benjamin Brewster (https://www.youtube.com/watch?v=1R9h-H2UnLs)

Signal Handlers: 
- CodeVault (https://www.youtube.com/watch?v=jF-1eFhyz1U)
- Class exploration: Signal handling API
- 3.3 Signals: Benjamin Brewster (https://www.youtube.com/watch?v=VwS3dx3uyiQ)

snprintf: CodeVault (https://www.youtube.com/watch?v=rUA3IkQNe5I)

dup2 and redirection: 
- CodeVault (https://www.youtube.com/watch?v=5fnVr-zH-SE&list=PLfqABt5AS4FkW5mOn2Tn9ZZLLDwA3kZUY&index=14)
- Class Exploration: Processes and I/O
- File Descriptors and exec(): The Linux Programming Interface, Ch 27, pg 575 - 578

waitpid and monitoring:
- CodeVault (https://www.youtube.com/watch?v=tcYo6hipaSA&list=PLfqABt5AS4FkW5mOn2Tn9ZZLLDwA3kZUY&index=2)
- Monitoring Child Processes: The Linux Programming Interface, Ch 26
- REAPING DEAD CHILDREN: The Linux Programming Interface, Ch 26, pg 556-557
- Class Exploration: Monitoring Child Processes
- Alerting to ending of background processes (https://stackoverflow.com/questions/7171722/how-can-i-handle-sigchld/7171836#7171836)
*/

// ================= THE STRUCT =================
typedef struct {
    char *prog;                     // program (command) name
    int num_args;                   // number of args provided
    char *args[MAX_ARGS];           // actual args provided
    char infile[MAX_FN_LEN];        // input file, if one is given
    char outfile[MAX_FN_LEN];       // output file, if one is given
} userIn;

// ================= FUNCTION PROTOTYPES =================
void display_struct(userIn);
void checkExp(userIn);
void expand(char [], pid_t); 
void free_userIn(userIn);
void prepUserIn(userIn*);
int cd(userIn);
int status(int, int);
void handle_sigtstp(int);
void execHandler(userIn, int *, int *, struct sigaction *, int *);
void handle_sigchld(int);

// ================= GLOBAL VARIABLE(S) =================
// to keep track of foreground/background mode
// Allow background by default (1)
int bg = 1;

// ================= FUNCTION DEFINITIONS =================
void prepUserIn(userIn *to_prep) {
    // Just to prepare/initialize userIn struct
    to_prep->num_args = 0;
    memset(to_prep->infile, '\0', MAX_FN_LEN);
    memset(to_prep->outfile, '\0', MAX_FN_LEN);
}

void free_userIn(userIn to_free) {
    // Frees dynamically allocated memory from userIn struct
    int i;
    free(to_free.prog);
    for (i = 0; i < to_free.num_args; i++) {
        free(to_free.args[i]);
    }
}

void get_input(userIn *stored_input, int *background) {
    // gets input from user and reads into 'line'
    // parses 'line' into command and arguments
    // looks for "<",">" and sets userIn.infile and userIn.outfile accordingly
    // looks for "&" and stores in *background flag
    char line[MAX_LINE];
    char *tok;
    int count = 0;

    // set stored_input.num_args to default 0
    prepUserIn(stored_input);

    // display prompt, flush stdout and  reset line to receive/print input
    printf(": ");
    fflush(stdout);
    memset(line, '\0', MAX_LINE);

    // read in line from stdin
    fgets(line, MAX_LINE, stdin);

    // replace '\n' with '\0'
    line[strlen(line)-1] = '\0';

    // nothing entered, set stored_input.prog to '\0' and return
    if (strlen(line) == 0) {
        stored_input->prog = '\0';
        return;
    }

    // if the line is a comment, set .prog to # and return
    if (line[0] == '#') {
        stored_input->prog = calloc(sizeof(char), 2);
        strncpy(stored_input->prog, "#", 1);
        return;
    }

    // otherwise, start splitting up input
    tok = strtok(line, " ");

    // store program name
    stored_input->prog = calloc(sizeof(char), strlen(tok)+1);
    strcpy(stored_input->prog, tok);
    
    // also need to put the program name as first argument in arg list
    stored_input->args[count] = calloc(sizeof(char), strlen(tok)+1);
    strcpy(stored_input->args[count], tok);
    count++;
    stored_input->num_args = count;

    // loop through to get the rest of the arguments
    while ((tok = strtok(NULL, " ")) != NULL) {

        // if it's an output redirect
        if (!strcmp(tok, ">")) {
            // get next tok and copy to outfile
            tok = strtok(NULL, " ");
            strcpy(stored_input->outfile, tok);
        } 

        // if it's an input redirect
        else if (!strcmp(tok, "<")) {
            // get next tok and copy to infile
            tok = strtok(NULL, " ");
            strcpy(stored_input->infile, tok);
        }

        // if it's "&" set background flag to 1
        // But do NOT add it to args
        else if (!strcmp(tok, "&")) {
            *background = 1;
        }

        else {
            // just store tok in the args like normal
            stored_input->args[count] = calloc(sizeof(char), strlen(tok)+1);
            strcpy(stored_input->args[count], tok);
            count++;
            stored_input->num_args = count;
        }
        
        
    }

    // Need to add a NULL to argument 'vector'
    stored_input->args[count] = tok;

}

void display_struct(userIn to_display) {
    // for debugging
    // prints all values currently stored in struct
    int i;

    printf("======= DISPLAYING STRUCT =======\n");
    if (to_display.prog == NULL) {
        printf("NULL");
    } else {
        printf("PROG: %s\n", to_display.prog);
    }

    printf("NUM_ARGS: %d\n", to_display.num_args);

    for (i = 0; i < to_display.num_args; i++) {
        printf("ARG %d: %s\n", i, to_display.args[i]);
    }

    printf("INFILE: %s\n", to_display.infile);
    printf("Outfile: %s\n", to_display.outfile);
    printf("======= DONE =======\n");
}

int cd(userIn userInput) {
    /*
    Changes current working directory
    If 1 arg is given -> Change directory to path given in userInput.args[0]
    If ONLY 'cd' is entered, chdir to HOME in env
    If too many args are entered, alert user, don't chdir
    */

    // set aside space for the directory to change to
    char *next = calloc(sizeof(char), MAX_DIR_LEN+1);

    // if ONLY cd was entered, go home set in ENV
    if (userInput.num_args == 1) {
        strcpy(next, getenv("HOME"));

    // if we have one argument, set 'next' to it
    } else if (userInput.num_args == 2) {
        strcpy(next, userInput.args[1]);

    // otherwise, we must have too many arguments, free and return -1
    } else {
        printf("Error: Too many arguments.\n");
        free(next);
        return -1;
    }

    // try to chdir
    // if we can't, alert user, free, and return -1
    if (chdir(next)) {
        printf("%s: No such file or directory\n", next);
        free(next);
        return -1;
    };
    
    // Another free because I kept failing valgrind without it
    free(next);
    return 0;

}

int status(int child_status, int chPid) {
    // Prints exit status of an exited/terminated process

    // Check if it was exited
    if (WIFEXITED(child_status)) {
        printf("%d exited, status=%d\n", chPid, WEXITSTATUS(child_status));
        fflush(stdout);
    
    // check if it was terminated
    } else if (WIFSIGNALED(child_status)) {
        printf("%d killed by signal %d (%s)\n", chPid, WTERMSIG(child_status), strsignal(WTERMSIG(child_status)));
        fflush(stdout);
    }
}

void shutdown(void) {
    // cleans up any remainig child processess
    pid_t terminated;
    // waits for any PID, stores in 'terminated' and alerts user
    while ((terminated = waitpid(-1, NULL, WNOHANG)) >0) {
        printf("Process %d terminated.\n", terminated);
        fflush(stdout);
        continue;
    }
    // terminate self
    waitpid(getpid(), NULL, WNOHANG);
}

void expand(char to_expand[], pid_t processId) {
    // Expands "$$" to the current PID

    char strOut[256];               // to store resultant char arry
    char strPid[256];               // to store char array of PID int
    memset(strPid, '\0', 256);      // set to null
    memset(strOut, '\0', 256);      // set to null
  
    int i, j, count;
  
    // Used snprintf to convert PID number to PID char array
    snprintf(strPid, 256, "%d", processId);
  
    count = 0;
    
    // Walk through char array containing "$$"
    for (i = 0; i<strlen(to_expand); i++) {

        // If we encounter "$$"...
        if (to_expand[i] == '$' && to_expand[i+1] == '$') {
        
        //copy strPid into strOut
        for (j = 0; j<strlen(strPid); j++) {
            strOut[count] = strPid[j];
            count++;
        }
        
        // increment to next valid char
        i += 1;
        
        } else {

        // otherwise, just copy from expanding string to strOut
        strOut[count] = to_expand[i];
        count++;
        }
    }
  // copy strOut back into buffer from whence it came
  strcpy(to_expand, strOut);
}

void checkExp(userIn to_check) {
    // Checks for any args that need to be expanded
    // and expands them
    int i;
    pid_t currPid= getpid();

    // Loop through args in struct
    for (i = 0; i< to_check.num_args; i++) {

        // if it contains "$$", expand it
        if (strstr(to_check.args[i], "$$") != NULL ) {
            expand(to_check.args[i], currPid);
        }
    }
}

void execHandler(userIn to_execute, int *background, int *stat, struct sigaction *sigint, int *spid) {
    // Executes a command, using the struct storing command and args
    // Uses *background to determine if command should be run with NOHANG or not
    // stores wait / exit status in *stat
    // pass in *sigint so we can change it from 'ignore' to 'default
    pid_t childPid = -2;
    pid_t bgPid = -2;

    // File descriptors for infile and outfile
    int fd_in;
    int fd_out;

    // Create fork
    childPid = fork();
    switch(childPid) {
        case(-1):
            
            // uh-oh
            perror("Unable to fork");
            exit(1);
            break;
        case(0): 
            
            // In CHILD process

            // If running in foreground, restore SIGINT default
            if (!(bg && *background)) {
                sigint->sa_handler = SIG_DFL;
                sigaction(SIGINT, sigint, NULL);
            }
            
            
            

            // check for an infile
            if (strlen(to_execute.infile) != 0) {
                // set stdin to infile filename
                if ((fd_in = open(to_execute.infile, O_RDONLY)) == -1) {
                    perror("Unable to open infile");
                    exit(1);
                }
                // redirect stdin to fd_in
                dup2(fd_in, STDIN_FILENO);
                // close on exec
                fcntl(fd_in, F_SETFD, FD_CLOEXEC);
            }

            // check for an outfile
            if (strlen(to_execute.outfile) != 0) {
                // set stdout to outfile filename
                if ((fd_out = open(to_execute.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0777)) == -1) {
                    perror("Unable to open outfile");
                    exit(1);
                }
                
                // redirect stdout to fd_out
                dup2(fd_out, STDOUT_FILENO);
                
                // close on exec
                fcntl(fd_out, F_SETFD, FD_CLOEXEC);
            }
            // try to execvp, exit if error
            if (execvp(to_execute.prog, to_execute.args) == -1) {
                perror("Unable to execute command");
                exit(2);
            };
        default: ;
            // In PARENT process

            // set up SIGCHLD handler for when a process completes
            struct sigaction sa;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sa.sa_handler = handle_sigchld;
            

            // Determine foreground/background of process
            // if we are NOT in foreground only mode (bg == 1)
            // AND the user passed an "&" (*background == 1)
            if (bg && *background) {
                
                printf("Background PID: %d\n", childPid);
                fflush(stdout);

                // let the user know when 
                // background child is done.
                sigaction(SIGCHLD, &sa, NULL);
                
                
            } else {
                
                // Trying to output approprate 'terminated' message
                sigaction(SIGCHLD, &sa, NULL);
                signal(SIGKILL, &handle_sigchld);
                // wait to complete
                *spid = waitpid(-1, stat, 0);
                
                
            }
    }
    
}

void handle_sigtstp(int num) {
    // MUST accept exactly one integer
    // sets global 'bg' to enter or exit foreground only mode
    // Need to use 'write' to alert user as it is 're-entrant'
    if (bg) {
        char *alert = "\nEntering foreground only (& is ignored)\n";
        write(STDOUT_FILENO, alert, strlen(alert));
        fflush(stdout);
        bg = 0;                 // Flip to false for next time
    } else {
        char *alert = "\nExiting foreground only mode\n";
        write(STDOUT_FILENO, alert, strlen(alert));
        fflush(stdout);
        bg = 1;                 // Flop to true for next time
    }

}

void handle_sigchld(int num) {
    pid_t pid;
    int stat;
    while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
        status(stat, pid);
        // printf("Process %d has ended.\n", pid);
        // fflush(stdout);
    }
}

int main(void) {

     // user input struct
    userIn user_input;
    int success;
    int childStatus;
    int bg_cmd = 0;
    int spawn = -5;


    // the following signal handler is straight from exploration notes
    // Initialize SIGINT_action struct to be empty
	struct sigaction SIGINT_action = {0};

	// Fill out the SIGINT_action struct
	// Register SIG_IGN as handler (ignore signal in main)
	SIGINT_action.sa_handler = SIG_IGN;
	// Block all catchable signals while handle_SIGINT is running
	sigfillset(&SIGINT_action.sa_mask);
	// No flags set
	SIGINT_action.sa_flags = 0;

	// Install our signal handler
    sigaction(SIGINT, &SIGINT_action, NULL);

    // Trying to follow SIGINT example from above, 
    // but redirect SIGTSTP, instead of ignoring
    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_sigtstp;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = 0;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
    
    while (1) {

        // Get user input into struct and set bg flag
        get_input(&user_input, &bg_cmd);

        // If input was blank or comment (#), do nothing
        if (user_input.prog == NULL || !strncmp(user_input.prog, "#", 1)) {
            continue;
        }

        // ############# FOR DEBUGGING #################
        // display_struct(user_input);

        // check for any arguments that need to be expanded ("$$" -> PID)
        checkExp(user_input);


        // Check for custom functions
        if (strcmp(user_input.prog, "exit") == 0) {
            
            break;
        
        } else if (strcmp(user_input.prog, "cd") == 0) {
            
            cd(user_input);

        } else if (strcmp(user_input.prog, "status") == 0) {
            
            status(childStatus, spawn);
        
        } else {

            // Otherwise, fork and exec
            execHandler(user_input, &bg_cmd, &childStatus, &SIGINT_action, &spawn);
        }
        
        // reset background command to 0
        // Or else every command thereafter 
        // will be a background command
        bg_cmd = 0;
        
        
        // Free userIn struct for the next loop
        free_userIn(user_input);

    }
   
    // One final free
    free_userIn(user_input);
    
    // Clean up any running processes and terminate 
    shutdown();

    return 0;
}