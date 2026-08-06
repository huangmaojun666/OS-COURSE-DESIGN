#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char *separators = " -\r\t\n./,";

static void
check_number(int number, int has_digit, int valid)
{
  if(valid && has_digit &&
     (number % 5 == 0 || number % 6 == 0)){
    printf("%d\n", number);
  }
}

static void
sixfive(int fd)
{
  char ch;
  int number = 0;
  int has_digit = 0;
  int valid = 1;
  int n;

  while((n = read(fd, &ch, 1)) == 1){
    if(ch >= '0' && ch <= '9'){
      if(valid){
        number = number * 10 + (ch - '0');
        has_digit = 1;
      }
    } 
    else if(strchr(separators, ch) != 0){
      check_number(number, has_digit, valid);

      number = 0;
      has_digit = 0;
      valid = 1;
    } 
    else {
      // 非数字且不是合法分隔符，当前字段整体无效。
      number = 0;
      has_digit = 0;
      valid = 0;
    }
  }

  if(n < 0){
    fprintf(2, "sixfive: read error\n");
  }

  // 文件末尾相当于一个隐含的分隔符。
  check_number(number, has_digit, valid);
}

int
main(int argc, char *argv[])
{
  int fd;

  if(argc < 2){
    fprintf(2, "usage: sixfive file...\n");
    exit(1);
  }

  for(int i = 1; i < argc; i++){
    fd = open(argv[i], O_RDONLY);

    if(fd < 0){
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      continue;
    }

    sixfive(fd);
    close(fd);
  }

  exit(0);
}