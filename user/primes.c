#include "../kernel/types.h"
#include "../user/user.h"

void primeproc(int *fd)
{
    close(fd[1]);
    int n, next_pipefd[2];

    pipe(next_pipefd);

    if (read(fd[0],&n, sizeof(int))== 4)
    {
        printf("prime %d\n",n);
        int child_pid = fork();
        if (child_pid !=0)
        {
            primeproc(next_pipefd);
            exit(0);
        }
        else
        {
            close(next_pipefd[0]);
            int temp;
            while (read(fd[0], &temp, sizeof(int))==4)
            {
                if ((temp % n) != 0)
                {
                    write(next_pipefd[1], &temp, sizeof(int));
                }
                
            }
            close(next_pipefd[1]);
            wait(0);
            

        }
        
    }
    
}


int main(int argc, char const *argv[])
{
    int pipefd[2], err;
    if ((err = pipe(pipefd)) < 0)
    {
        fprintf(2,"cannot create a pipe\n");
        exit(-1);
    }
    int c_pid = fork();
    if (c_pid == 0)
    {
        primeproc(pipefd);
        exit(0);
    }
    close(pipefd[0]);
    
    int limit =35;
    if (argc == 2)
    {
        limit = atoi(argv[1]);
    }
    for (int i = 2; i < limit; i++)
    {
        int write_bytes = 0;
        if((write_bytes = write(pipefd[1],&i, sizeof(int) ))!= 4){
            fprintf(2,"cannot write int %d to pipe, %d bytes",i,write_bytes);
        }
    }
    close(pipefd[1]);
    wait(0);

    exit(0);

}
