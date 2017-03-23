#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/wait.h>


int main(int argc, char *argv[]) {
    if (argc <= 1) {
        printf("Usage: piper <command>\n");
        return 1;

    }
    
    int fds[2];
    pipe(fds);
    
    int pid;
    if((pid = fork()) == -1)
    {
        perror("fork");
        return 1;
    }
    
    if(pid == 0) {
        // We can close the read side of the pipe on the child
        close(fds[0]);
        char pipeStr[20];
        snprintf(pipeStr, 20, "%d", fds[1]);

        char **cmd = argv + 1;
        
        if (strstr(argv[1], "-m") != NULL) {
            // wrap with mnexec
            char *baseCmd[1000] = { "./mnexec", "-cnPu", "-w" };
            baseCmd[3] = pipeStr;
            memcpy(baseCmd + 4, argv + 1, (argc-1) * sizeof(char*));
            cmd = baseCmd;
        }

        printf("Command: ");
        for (char **ptr = cmd; *ptr != NULL; ptr++) printf("%s ", *ptr);
        printf("\n");

        execvp(cmd[0], cmd);
        perror(cmd[0]);
        return 1;
    }
    
    // Parent reads pipe
    int n;
    char buf[10];
    n = read(fds[0], buf, sizeof(buf));
    fprintf(stderr, "Got PID: '%s'\n", buf);
    int status;
    wait(&status);
    return 0;
}
