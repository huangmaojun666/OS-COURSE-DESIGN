#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"
//返回路径最后一部分
static char *
basename(char *path)
{
  char *p;

  p = path + strlen(path);

  while(p > path && *(p - 1) != '/')
    p--;

  return p;
}

/*
 * 对找到的文件执行命令。
 */
static void
run_command(char *path, char **command, int command_argc)
{
  char *exec_argv[MAXARG];
  int pid;
  int i;

  /*
   * 除原有参数外，还需要两个位置：
   * 1. 保存匹配文件的路径
   * 2. 保存结尾的空指针
   */
  if(command_argc + 2 > MAXARG){
    fprintf(2, "find: too many exec arguments\n");
    return;
  }

  for(i = 0; i < command_argc; i++)
    exec_argv[i] = command[i];

  // 把匹配文件的路径追加到命令参数末尾。
  exec_argv[command_argc] = path;

  // exec参数数组必须以空指针结束。
  exec_argv[command_argc + 1] = 0;

  pid = fork();

  if(pid < 0){
    fprintf(2, "find: fork failed\n");
    return;
  }

  if(pid == 0){
    // 子进程执行指定命令。
    exec(exec_argv[0], exec_argv);

    // exec成功时不会返回，执行到这里说明失败。
    fprintf(2, "find: exec %s failed\n", exec_argv[0]);
    exit(1);
  }

  // 父进程等待子进程执行结束。
  wait(0);
}

/*
 * 从path开始递归查找名称为target的文件。
 * exec_mode == 0：
 *   只输出匹配文件路径。
 * exec_mode == 1：
 *   对匹配文件执行command。
 */
static void
find(char *path, char *target, int exec_mode,
     char **command, int command_argc)
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

  // 当前路径名称与目标名称相同。
  if(strcmp(basename(path), target) == 0){
    if(exec_mode)
      run_command(path, command, command_argc);
    else
      printf("%s\n", path);
  }

  // 普通文件和设备文件不需要继续递归。
  if(st.type != T_DIR){
    close(fd);
    return;
  }

  /*
   * 检查缓冲区空间：
   * 当前路径 + "/" + 目录项名称 + "\0"
   */
  if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
    fprintf(2, "find: path too long\n");
    close(fd);
    return;
  }

  strcpy(buf, path);

  // p指向当前路径字符串的末尾。
  p = buf + strlen(buf);

  // 添加路径分隔符。
  *p++ = '/';

  // 每次读取一个目录项。
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    // inode编号为0表示未使用的目录项。
    if(de.inum == 0)
      continue;

    /*
     * 把目录项名称复制到路径末尾。
     * xv6目录项名称固定占用DIRSIZ个字节。
     */
    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = '\0';

    // 避免进入当前目录和父目录，防止无限递归。
    if(strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
      continue;

    // 递归处理子文件或子目录。
    find(buf, target, exec_mode, command, command_argc);
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  int exec_mode = 0;
  char **command = 0;
  int command_argc = 0;
  if(argc == 3){
    exec_mode = 0;
  } else if(argc >= 5 && strcmp(argv[3], "-exec") == 0){
    exec_mode = 1;
    command = &argv[4];
    command_argc = argc - 4;
  } else {
    fprintf(2,
            "usage: find path filename [-exec command arguments...]\n");
    exit(1);
  }

  find(argv[1], argv[2],
       exec_mode, command, command_argc);

  exit(0);
}