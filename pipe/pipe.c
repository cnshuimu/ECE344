#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

int *w;
int count = 0;

static void check_error(int ret, const char *message) {
    if (ret != -1) {
        return;
    }
    int err = errno;
    perror(message);
    exit(err);
}

static void parent(int in_pipefd[2], pid_t child_pid, int num, int total) {
    if (num < total - 1){
       check_error(dup2(in_pipefd[0], STDIN_FILENO), "dup2 1");
    }
    check_error(close(in_pipefd[0]), "close1");
    check_error(close(in_pipefd[1]), "close2"); 
    w[num-1] = child_pid;
    count++;
}

static void child(int in_pipefd[2], const char *program, int num, int total) {   
    if (num < total - 1){
        check_error(dup2(in_pipefd[1], STDOUT_FILENO), "dup2 2"); 
    }
    check_error(close(in_pipefd[0]), "close3");        
    check_error(close(in_pipefd[1]), "close4");

    check_error(execlp(program, program, NULL), "execlp");
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        return EINVAL;
    }
    
    w = malloc(sizeof(int)*(argc));

    for (int i = 1; i < argc; i++){
        int pipefd[2] = {0};
        check_error(pipe(pipefd), "pipe");

        pid_t pid = fork();
        check_error(pid, "child");
        
        if (pid > 0){
            parent(pipefd, pid, i, argc);
        }
        else{
            child(pipefd, argv[i], i, argc);
        } 
    }
    for (unsigned int j = 0; j < count; j++){
        int wstatus;
        int child_pid = w[j];
        check_error(waitpid(child_pid, &wstatus, 0), "wait");
        assert(WIFEXITED(wstatus));
        int exit_status = WEXITSTATUS(wstatus);
        if (exit_status != 0){
            return WEXITSTATUS(wstatus);
        }
    }
    
    return 0;
}



