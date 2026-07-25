#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "overlayrb.h"

#define HSH_RD_BUFSIZE 1024
#define HSH_TOK_BUFSIZE 64
#define HSH_TOK_DELIM " \t\r\n\a"

#define HSH_BLOCK_BUFSIZE 32
#define HSH_BLOCK_DELIM "|>&"

typedef enum Operator{
    OP_PIPE,
    OP_REDIR,
    OP_AND,
    OP_OR,
    OP_BG,
    OP_NONE
}Operator;

void hsh_loop();
char *hsh_read_line();
char **hsh_split_line(char *line);
int hsh_cd(char **args);
int hsh_help(char **args);
int hsh_exit(char **args);
int hsh_execute_line(char **args, Operator op, int opcount);
int hsh_num_builtins();
int hsh_launch(char **args);
void hsh_cleanup();

Operator parse_ops(char *line, int *opcount);
char **parse_block(char *block);

int handle_pipe(char **blocks, int pipecount);
int handle_redir(char *block, char *out);
int handle_and(char *block1, char *block2);
int handle_or(char *block1, char *block2);
int handle_bg(char *block);
int handle_op_none(char *block);

void free_args(char **args);
int is_builtin(char *name);
int exec_builtin(char **args, int index);

char *builtin_str[] = {"cd", "help", "exit", "undo"};
int (*builtin_func[])(char **) = {hsh_cd,  hsh_help, hsh_exit, undo};

pid_t bg_jobs[120];
int bg_count = 0;

void sigchld_handler(int signum){
    (void)signum;

    if(bg_count == 0) return;
    int status;
    int tmp_count = bg_count;

    for(int i = 0; i < tmp_count; i++){
        if(waitpid(bg_jobs[i], &status, WNOHANG) > 0){
            bg_jobs[i] = bg_jobs[bg_count - 1];
            bg_count--;
        }
    }
}

void init_sigs(){
    struct sigaction ign_sig = {0};
    ign_sig.sa_handler = SIG_IGN;
    ign_sig.sa_flags = 0;

    sigaction(SIGTSTP, &ign_sig, NULL);
    sigaction(SIGINT, &ign_sig, NULL);

    struct sigaction sigchld = {0};
    sigchld.sa_flags = SA_RESTART;
    sigchld.sa_handler = sigchld_handler;

    sigaction(SIGCHLD, &sigchld, NULL);
}

int main(){

    init_sigs();

    if(init_ovl_dirs() != 0){
        fprintf(stderr, "hsh: Failed to start\n");
        return EXIT_FAILURE;
    }

    hsh_loop();

    hsh_cleanup();

    return EXIT_SUCCESS;
}

void hsh_loop(){

    char *line;
    int status, opcount;
    Operator op;
    char **blocks;

    do{
        printf("\n> ");
        line = hsh_read_line();
        op = parse_ops(line, &opcount);
        blocks = hsh_split_line(line);
        status = hsh_execute_line(blocks, op, opcount);

        free(line);
        free_args(blocks);
    }while(status != -1);
}


char *hsh_read_line(){

    int bufsize = HSH_RD_BUFSIZE;
    int position = 0;
    char *buffer = malloc(sizeof(char) * bufsize);
    int c;

    if(!buffer){
        fprintf(stderr, "hsh: allocation error\n");
        exit(EXIT_FAILURE);
    }

    while(1){
        c = getchar();

        if(c == EOF || c == '\n'){
            buffer[position] = '\0';
            return buffer;
        }
        else{
            buffer[position] = c;
        }
        position++;

        if(position >= bufsize){
            bufsize += HSH_RD_BUFSIZE;
            char *temp = realloc(buffer, bufsize);
            if(!temp){
                fprintf(stderr, "hsh: allocation error\n");
                exit(EXIT_FAILURE);
            }
            buffer = temp;
        }
    }
}

char **hsh_split_line(char *line){

    int bufsize = HSH_BLOCK_BUFSIZE;
    int position = 0;
    char **blocks = malloc(bufsize * sizeof(char*));
    char *block;

    if(!blocks){
        fprintf(stderr, "hsh: allocation error\n");
        exit(EXIT_FAILURE);
    }

    char *dup_line = strdup(line);

    block = strtok(dup_line, HSH_BLOCK_DELIM);

    while(block != NULL){
        blocks[position] = strdup(block);
        position++;

        if(position >= bufsize){
            bufsize += HSH_BLOCK_BUFSIZE;
            char **temp = realloc(blocks, bufsize * sizeof(char*));
            if(!temp){
                fprintf(stderr, "hsh: allocation error\n");
                exit(EXIT_FAILURE);
            }
            blocks = temp;
        }

        block = strtok(NULL, HSH_BLOCK_DELIM);
    }

    blocks[position] = NULL;
    free(dup_line);
    return blocks;
}

int hsh_execute_line(char **blocks, Operator op, int opcount){
    switch(op){
        case OP_PIPE:
            return handle_pipe(blocks, opcount);

        case OP_REDIR:
            return handle_redir(blocks[0], blocks[1]);

        case OP_AND:
            return handle_and(blocks[0], blocks[1]);

        case OP_OR:
            return handle_or(blocks[0], blocks[1]);

        case OP_BG:
            return handle_bg(blocks[0]);

        case OP_NONE:
            return handle_op_none(blocks[0]);
    }

    fprintf(stderr, "hsh: operator not found\n");
    return 1;
}

int hsh_launch(char **args){
    if(args == NULL) return 1;

    int index = is_builtin(args[0]);
    if(index >= 0){
        return exec_builtin(args, index);
    }

    pid_t pid;
    int status;

    pid = fork();
    if(pid == 0){
        setpgid(0, 0);

        struct sigaction sa = {0};
        sa.sa_handler = SIG_DFL;
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        uid_t uid = getuid();
        gid_t gid = getgid();
        if(init_child_ovl(uid, gid) != 0){
            fprintf(stderr, "hsh: failed to init child ovl\n");
            exit(EXIT_FAILURE);
        }

        execvp(args[0], args);
        fprintf(stderr, "hsh: %s: command not found\n", args[0]);
        exit(EXIT_FAILURE);
    }
    else if(pid < 0){
        perror("hsh: fork failed");
        return 1;
    }
    else{
        setpgid(pid, pid);
        tcsetpgrp(STDIN_FILENO, pid);

        waitpid(pid, &status, WUNTRACED);
        signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(STDIN_FILENO, getpgid(0));
    }

    reap_overlay();

    if(WIFEXITED(status)){
        return WEXITSTATUS(status);
    }

    return 1;
}

int hsh_num_builtins(){
    return sizeof(builtin_str) / sizeof(char*);
}

int hsh_cd(char **args){
    if(args[0] == NULL){
        fprintf(stderr, "hsh: expected argument to \"cd\"\n");
    }
    else{
        if(chdir(args[1]) != 0){
            perror("hsh");
            return 1;
        }
    }
    return 0;
}

int hsh_help(char **args){
    (void)args;
    printf("This is the hollow shell.\n");
    printf("The following commands are built in:\n");

    for(int i = 0; i < hsh_num_builtins(); i++){
        printf(" %s\n", builtin_str[i]);
    }

    return 0;
}

int hsh_exit(char **args){
    (void)args;
    return -1;
}

Operator parse_ops(char *line, int *opcount){
    int count = 0;
    Operator op = OP_NONE;
    for(int i = 0; line[i] != '\0'; i++){
        if(line[i] == '|'){
            if(line[i + 1] && line[i + 1] == '|'){
                if(op != OP_NONE){
                    fprintf(stderr, "hsh: Invalid operator sequence\n");
                    return -1;
                }

                op = OP_OR;
                count++;
                i++;
                continue;

            }
            op = OP_PIPE;
            count++;

        }

        if(line[i] == '>'){
            if(op != OP_NONE){
                fprintf(stderr, "hsh: Invalid operator sequence\n");
                return -1;
            }
            op = OP_REDIR;

        }

        if(line[i] == '&'){
            if(line[i + 1] && line[i + 1] == '&'){
                if(op != OP_NONE){
                    fprintf(stderr, "hsh: Invalid operator sequence\n");
                    return -1;
                }
                op = OP_AND;
                count++;
                i++;
            }
            else{
                if(op != OP_NONE){
                    fprintf(stderr, "hsh: Invalid operator sequence\n");
                }
                op = OP_BG;
                count++;
            }
        }
    }

    *opcount = count;
    return op;
}

char **parse_block(char *block){
    if(block == NULL) return NULL;

    char *dup_block = strdup(block);

    int bufsize = HSH_TOK_BUFSIZE;
    char **tokens = malloc(bufsize * sizeof(char*));
    int index = 0;

    char *token = strtok(dup_block, HSH_TOK_DELIM);
    while(token != NULL){
        tokens[index] = strdup(token);
        index++;

        if(index >= HSH_TOK_BUFSIZE){
            bufsize += bufsize;
            char **temp = realloc(tokens, bufsize * sizeof(char*));

            if(!temp){
                fprintf(stderr, "hsh: allocation failed\n");
                exit(EXIT_FAILURE);
            }

            tokens = temp;
        }

        token = strtok(NULL, HSH_TOK_DELIM);
    }

    tokens[index] = NULL;
    free(dup_block);
    return tokens;
}

int handle_pipe(char **blocks, int pipecount){
    if(blocks == NULL) return 1;

    int fd[2];
    int in_fd = STDIN_FILENO;
    int num_commands = pipecount + 1;

    for(int i = 0; i < num_commands; i++){
        if(blocks[i] == NULL || blocks[i][0] == '\0'){
            fprintf(stderr, "hsh: Syntax error\n");
            return 1;
        }
    }

    for(int i = 0; i < num_commands; i++){
        if(num_commands - 1 > i){
            if(pipe(fd) != 0){
                perror("hsh: pipe failed");
                return 1;
            }
        }

        char **args = parse_block(blocks[i]);

        pid_t pid = fork();
        if(pid == 0){
            if(in_fd != STDIN_FILENO){
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }

            if(num_commands - 1 > i){
                dup2(fd[1], STDOUT_FILENO);
                close(fd[1]);
                close(fd[0]);
            }

            int index = is_builtin(args[0]);
            if(index >= 0){
                int exec_status = exec_builtin(args, index);
                exit(exec_status);
            }

            execvp(args[0], args);
            fprintf(stderr, "hsh: %s: command not found\n", args[0]);
            exit(EXIT_FAILURE);
        }
        else if(pid < 0){
            perror("hsh: fork failed");
            return 1;
        }
        else{
            if(in_fd != STDIN_FILENO){
                close(in_fd);
            }

            if(num_commands - 1 > i){
                in_fd = fd[0];
                close(fd[1]);
            }

            free_args(args);
        }
    }

    int status;
    pid_t wpid = wait(&status);
    int exit_code = 0;
    while(wpid != -1){
        if(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0)){
            exit_code = 1;
        }

        wpid = wait(&status);
    }

    return exit_code;
}

int handle_redir(char *block, char *out_block){
    char **args = parse_block(block);
    char **out = parse_block(out_block);
    if(args == NULL || out == NULL) return 1;

    pid_t pid = fork();

    if(pid == 0){

        if(init_child_ovl(getuid(), getgid()) != 0){
            fprintf(stderr, "hsh: failed to init child overlay\n");
            exit(EXIT_FAILURE);
        }

        int fd = open(*out, O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if(fd < 0){
            perror("hsh: failed to open file");
            exit(EXIT_FAILURE);
        }

        dup2(fd, STDOUT_FILENO);
        close(fd);

        int index = is_builtin(args[0]);
        if(index >= 0){
            int exec_status = exec_builtin(args, index);
            exit(exec_status);
        }

        execvp(args[0], args);
        fprintf(stderr, "hsh: %s: command not found\n", args[0]);
        exit(EXIT_FAILURE);
    }
    else if(pid < 0){
        perror("hsh: fork failed");
        return 1;
    }

    free_args(args);
    free_args(out);

    int status;
    waitpid(pid, &status, 0);

    reap_overlay();

    // Child terminated normally
    if(WIFEXITED(status)){
        return WEXITSTATUS(status);
    }

    //child terminated by a signal
    return 1;
}

int handle_and(char *block1, char *block2){
    char **args1 = parse_block(block1);
    char **args2 = parse_block(block2);
    if(args1 == NULL || args2 == NULL) return 1;

    int status = hsh_launch(args1);

    if(status == 0)
        status = hsh_launch(args2);

    free_args(args1);
    free_args(args2);
    return status;
}

int handle_or(char *block1, char *block2){
    char **args1 = parse_block(block1);
    char **args2 = parse_block(block2);
    if(args1 == NULL || args2 == NULL) return 1;

    int status = hsh_launch(args1);

    if(status != 0)
        status = hsh_launch(args2);

    free_args(args1);
    free_args(args2);
    return status;
}

void free_args(char **args){
    if(args == NULL) return;

    for(int i = 0; args[i] != NULL; i++){
        free(args[i]);
    }
    free(args);
}

int handle_op_none(char *block){
    char **args = parse_block(block);
    if(args == NULL) return 1;

    int status = hsh_launch(args);

    free_args(args);
    return status;
}

int is_builtin(char *name){
    if(name == NULL) return -1;

    for(int i = 0; i < hsh_num_builtins(); i++){
        if(strcmp(name, builtin_str[i]) == 0){
            return i;
        }
    }

    return -1;
}

int exec_builtin(char **args, int index){
    if(args == NULL || index < 0) return 1;

    return builtin_func[index](args);
}

int handle_bg(char *block){
    char **args = parse_block(block);

    int idx = is_builtin(args[0]);
    if(idx >= 0){
        return exec_builtin(args, idx);
    }

    pid_t pid = fork();
    int status;
    if(pid == 0){
        setpgid(0, 0);

        if(init_child_ovl(getuid(), getgid()) != 0){
            fprintf(stderr, "hsh: failed to init child overlay\n");
            exit(EXIT_FAILURE);
        }

        execvp(args[0], args);
        exit(EXIT_FAILURE);
    }
    else if(pid < 0){
        fprintf(stderr, "hsh: Error forking\n");
        return 1;
    }
    else{
        setpgid(pid, pid);

        waitpid(pid, &status, WNOHANG);

        signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(STDIN_FILENO, getpgid(0));
    }

    reap_overlay();

    if(WIFEXITED(status)){
        return WEXITSTATUS(status);
    }

    bg_jobs[bg_count] = pid;
    bg_count++;

    return 1;
}

void hsh_cleanup(){
    for(int i = 0; i < bg_count; i++){
        kill(-bg_jobs[i], SIGTERM);
    }

    int stat;
    while(waitpid(-1, &stat, WNOHANG) != -1){
        continue;
    }

    ovl_cleanup();
}
