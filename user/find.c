#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char *
basename(char *path)
{
  char *p;

  p = path + strlen(path);

  while(p > path && *(p - 1) != '/')
    p--;

  return p;
}

static void
find(char *path, char *target)
{
  char buf[512];
  char *p;
  int fd;
  struct stat st;
  struct dirent de;

  fd = open(path, O_RDONLY);
  if(fd < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  // 检查当前路径最后一部分的名称。
  if(strcmp(basename(path), target) == 0)
    printf("%s\n", path);

  // 普通文件不需要继续递归。
  if(st.type != T_DIR){
    close(fd);
    return;
  }

  // 原路径、斜杠、目录项名称和字符串结束符。
  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
    fprintf(2, "find: path too long\n");
    close(fd);
    return;
  }

  strcpy(buf, path);
  p = buf + strlen(buf);
  *p++ = '/';

  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    // inode编号为0表示未使用的目录项。
    if(de.inum == 0)
      continue;

    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = '\0';

    // 避免递归进入当前目录和父目录。
    if(strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
      continue;

    find(buf, target);
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "usage: find path filename\n");
    exit(1);
  }

  find(argv[1], argv[2]);

  exit(0);
}