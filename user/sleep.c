#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "../user/user.h"

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDER_FILENO 2

int 
main(int argc, char *argv[])
{
    if(argc != 2)
    {
        fprintf(STDER_FILENO, "usage: sleep <number>\n");
        exit(1);
    }
    int sleep_sec = atoi(argv[1]);
    sleep(sleep_sec);
    exit(0);
}