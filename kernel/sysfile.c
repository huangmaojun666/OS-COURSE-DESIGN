//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 hint;
  int length;
  int prot;
  int flags;
  int fd;
  int offset;
  struct file *f;
  struct proc *p = myproc();

  argaddr(0, &hint);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  if(hint != 0 || length <= 0 || offset != 0)
    return (uint64)-1;

  if(flags != MAP_SHARED && flags != MAP_PRIVATE)
    return (uint64)-1;

  if((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
    return (uint64)-1;

  if(fd < 0 || fd >= NOFILE ||
     (f = p->ofile[fd]) == 0)
    return (uint64)-1;

  // 本实验只映射普通inode文件。
  if(f->type != FD_INODE || !f->readable)
    return (uint64)-1;

  // 共享可写映射要求文件以可写方式打开。
  if((flags == MAP_SHARED) &&
     (prot & PROT_WRITE) &&
     !f->writable)
    return (uint64)-1;

  // 查找空闲VMA槽位。
  struct vma *free_vma = 0;

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0){
      free_vma = &p->vmas[i];
      break;
    }
  }

  if(free_vma == 0)
    return (uint64)-1;

  /*
   * 找到所有现有映射的最高结束地址，
   * 新映射放在其后。
   */
  uint64 start = MMAPBASE;

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      uint64 end =
          PGROUNDUP(p->vmas[i].addr +
                    p->vmas[i].length);

      if(end > start)
        start = end;
    }
  }

  uint64 maplen = PGROUNDUP((uint64)length);

  if(start + maplen < start ||
     start + maplen >= TRAPFRAME)
    return (uint64)-1;

  free_vma->used = 1;
  free_vma->addr = start;
  free_vma->length = length;
  free_vma->prot = prot;
  free_vma->flags = flags;
  free_vma->offset = offset;

  /*
   * VMA持有独立的文件引用。
   * 即使用户随后close(fd)，映射仍然有效。
   */
  free_vma->file = filedup(f);

  /*
   * 此处不调用kalloc()，也不建立页表映射，
   * 物理页将在缺页异常中分配。
   */
  return start;
}

int
mmap_pagefault(struct proc *p, uint64 va, int cause)
{
  struct vma *v = 0;
  uint64 pageva = PGROUNDDOWN(va);

  // 查找包含故障地址的VMA。
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used &&
       va >= p->vmas[i].addr &&
       va < p->vmas[i].addr +
            p->vmas[i].length){
      v = &p->vmas[i];
      break;
    }
  }

  if(v == 0)
    return -1;

  /*
   * RISC-V：
   * 12：指令缺页
   * 13：读缺页
   * 15：写缺页
   */
  if(cause == 13 && !(v->prot & PROT_READ))
    return -1;

  if(cause == 15 && !(v->prot & PROT_WRITE))
    return -1;

  if(cause == 12 && !(v->prot & PROT_EXEC))
    return -1;

  // 防止对已映射页面重复调用mappages。
  pte_t *pte = walk(p->pagetable, pageva, 0);
  if(pte != 0 && (*pte & PTE_V))
    return -1;

  char *mem = kalloc();
  if(mem == 0)
    return -1;

  memset(mem, 0, PGSIZE);

  uint64 file_offset =
      v->offset + (pageva - v->addr);

  /*
   * 从文件读取一页数据。
   * 映射超过文件末尾时，剩余部分保持为0。
   */
  ilock(v->file->ip);

  int n = readi(v->file->ip,
                0,
                (uint64)mem,
                file_offset,
                PGSIZE);

  iunlock(v->file->ip);

  if(n < 0){
    kfree(mem);
    return -1;
  }

  int perm = PTE_U;

  if(v->prot & PROT_READ)
    perm |= PTE_R;

  if(v->prot & PROT_WRITE)
    perm |= PTE_W;

  if(v->prot & PROT_EXEC)
    perm |= PTE_X;

  if(mappages(p->pagetable,
              pageva,
              PGSIZE,
              (uint64)mem,
              perm) != 0){
    kfree(mem);
    return -1;
  }

  return 0;
}
static int
vma_writeback_page(struct vma *v,uint64 va,uint64 pa,uint64 n)
{
  uint64 offset =v->offset + (va - v->addr);

  begin_op();
  ilock(v->file->ip);

  // Do not let an mmap write-back extend the file. The final file page
  // may be only partially populated even when the VMA covers full pages.
  if(offset >= v->file->ip->size){
    iunlock(v->file->ip);
    end_op();
    return 0;
  }
  if(n > v->file->ip->size - offset)
    n = v->file->ip->size - offset;

  int result = writei(v->file->ip,0,pa,offset,n);

  iunlock(v->file->ip);
  end_op();

  return result == n ? 0 : -1;
}
int
vma_unmap(struct proc *p, uint64 addr, uint64 len)
{
  struct vma *v = 0;

  if(len == 0 || (addr % PGSIZE) != 0)
    return -1;

  if(addr + len < addr)
    return -1;

  // 查找完整包含解除区间的VMA。
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used &&
       addr >= p->vmas[i].addr &&
       addr + len <=
         p->vmas[i].addr +
         p->vmas[i].length){
      v = &p->vmas[i];
      break;
    }
  }

  if(v == 0)
    return -1;

  uint64 old_start = v->addr;
  uint64 old_end = v->addr + v->length;
  uint64 unmap_end = addr + len;

  /*
   * 实验保证只能解除VMA开头、末尾或整个区域，
   * 不允许在中间打洞。
   */
  if(addr != old_start && unmap_end != old_end)
    return -1;

  /*
   * 逐页处理。未发生缺页的页面没有实际映射，
   * 因此不需要释放。
   */
  for(uint64 a = addr;
      a < PGROUNDUP(unmap_end);
      a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);

    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    uint64 pa = PTE2PA(*pte);

    /*
     * MAP_SHARED的脏页需要写回文件。
     * PTE_D为RISC-V页表项中的dirty位。
     */
    if(v->flags == MAP_SHARED &&
       (*pte & PTE_D)){
      uint64 n = PGSIZE;

      if(a + n > unmap_end)
        n = unmap_end - a;

      if(a + n > old_end)
        n = old_end - a;

      if(vma_writeback_page(v, a, pa, n) < 0)
        return -1;
    }

    uvmunmap(p->pagetable, a, 1, 1);
  }

  /*
   * 根据解除范围调整VMA。
   */
  if(addr == old_start && unmap_end == old_end){
    struct file *f = v->file;

    memset(v, 0, sizeof(*v));
    fileclose(f);
  } else if(addr == old_start){
    // 解除开头部分。
    v->addr += len;
    v->offset += len;
    v->length -= len;
  } else {
    // 解除末尾部分。
    v->length = addr - old_start;
  }

  return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int length;

  argaddr(0, &addr);
  argint(1, &length);

  if(length <= 0)
    return -1;

  return vma_unmap(myproc(),addr,(uint64)length);
}

void
vma_unmap_all(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      uint64 addr = p->vmas[i].addr;
      uint64 len = p->vmas[i].length;
      vma_unmap(p, addr, len);
    }
  }
}