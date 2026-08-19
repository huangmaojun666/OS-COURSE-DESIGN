#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        return pte;
      }
#endif
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}
static pte_t *
walkleaf(pagetable_t pagetable,uint64 va,int *level)
{
  if(va >= MAXVA)
    panic("walkleaf");

  for(int l = 2; l > 0; l--){
    pte_t *pte =
      &pagetable[PX(l, va)];

    if(*pte & PTE_V){
      if(*pte & (PTE_R | PTE_W | PTE_X)){
        *level = l;
        return pte;
      }

      pagetable =
        (pagetable_t)PTE2PA(*pte);
    } else {
      *level = l;
      return pte;
    }
  }

  *level = 0;
  return &pagetable[PX(0, va)];
}
static pte_t *
walklevel1(pagetable_t pagetable,uint64 va,int alloc)
{
  if(va >= MAXVA)
    panic("walklevel1");

  pte_t *pte =
    &pagetable[PX(2, va)];

  if(*pte & PTE_V){
    if(*pte & (PTE_R | PTE_W | PTE_X))
      return 0;

    pagetable =
      (pagetable_t)PTE2PA(*pte);
  } else {
    if(!alloc)
      return 0;

    pagetable_t newpt =
      (pagetable_t)kalloc();

    if(newpt == 0)
      return 0;

    memset(newpt, 0, PGSIZE);

    *pte = PA2PTE(newpt) | PTE_V;
    pagetable = newpt;
  }

  return &pagetable[PX(1, va)];
}
// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable,
         uint64 va)
{
  pte_t *pte;
  uint64 pa;
  int level;

  if(va >= MAXVA)
    return 0;

  pte = walkleaf(pagetable,
                 va,
                 &level);

  if(pte == 0)
    return 0;

  if((*pte & PTE_V) == 0)
    return 0;

  if((*pte & PTE_U) == 0)
    return 0;

  pa = PTE2PA(*pte);

  if(level == 1){
    /*
     * walkaddr原有语义是返回当前4 KiB虚拟页
     * 对应的物理页基地址。
     *
     * 因此，需要加上该4 KiB页在2 MiB
     * 超级页中的偏移，但不能加页内低12位偏移。
     */
    pa += PGROUNDDOWN(va) -SUPERPGROUNDDOWN(va);
  } else if(level != 0){
    return 0;
  }

  return pa;
}
static void
vmprintwalk(pagetable_t pagetable,
            int level,
            uint64 va)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];

    if((pte & PTE_V) == 0)
      continue;

    uint64 currentva =
      va | ((uint64)i << PXSHIFT(level));
    uint64 pa = PTE2PA(pte);

    int depth = 3 - level;
    for(int j = 0; j < depth; j++)
      printf(" ..");

    printf("%p: pte %p pa %p\n",
           (void*)currentva,  (void*)pte,  (void*)pa);

    if(level > 0 &&
       (pte & (PTE_R | PTE_W | PTE_X)) == 0){
      vmprintwalk((pagetable_t)pa,
                  level - 1,
                  currentva);
    }
  }
}
#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void
vmprint(pagetable_t pagetable) {
   printf("page table %p\n",  (void*)pagetable);
   vmprintwalk(pagetable, 2, 0);
}
#endif



// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}
static int
supermappages(pagetable_t pagetable,uint64 va,uint64 pa,int perm)
{
  if((va % SUPERPGSIZE) != 0)
    panic("supermappages: va");

  if((pa % SUPERPGSIZE) != 0)
    panic("supermappages: pa");

  pte_t *pte =
    walklevel1(pagetable, va, 1);

  if(pte == 0)
    return -1;

  if(*pte & PTE_V)
    panic("supermappages: remap");

  *pte = PA2PTE(pa) |perm |PTE_V;
  return 0;
}
// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}
static int
superdemote(pagetable_t pagetable,
            uint64 va)
{
  uint64 base;
  uint64 oldpa;
  uint flags;
  pte_t *superpte;
  pagetable_t newpt;
  int level;
  int i;

  /*
   * 找到va所在的2 MiB超级页起始虚拟地址。
   */
  base = SUPERPGROUNDDOWN(va);

  /*
   * 查询当前映射，要求它是L1超级页叶子。
   */
  superpte =
    walkleaf(pagetable, base, &level);

  if(superpte == 0)
    return -1;

  if((*superpte & PTE_V) == 0)
    return -1;

  if(level != 1)
    return -1;

  if((*superpte &
      (PTE_R | PTE_W | PTE_X)) == 0)
    return -1;

  /*
   * 保存原超级页物理地址和权限。
   */
  oldpa = PTE2PA(*superpte);
  flags = PTE_FLAGS(*superpte);

  /*
   * 分配一个4 KiB物理页作为新的L0页表。
   */
  newpt =
    (pagetable_t)kalloc();

  if(newpt == 0)
    return -1;

  memset(newpt, 0, PGSIZE);

  /*
   * 为超级页中的512个4 KiB区域分别
   * 分配普通物理页，并复制原来的数据。
   */
  for(i = 0;
      i < 512;
      i++){
    char *mem = kalloc();

    if(mem == 0)
      goto err;

    memmove(mem,
            (void *)(oldpa +
                     (uint64)i * PGSIZE),
            PGSIZE);

    /*
     * newpt就是L0页表。
     * 每一个newpt[i]都是一个4 KiB叶子PTE。
     */
    newpt[i] =
      PA2PTE((uint64)mem) |
      flags;
  }

  /*
   * 原superpte是L1叶子PTE。
   *
   * 现在将它改为非叶子PTE，使其指向
   * 新创建的L0页表。
   *
   * 非叶子PTE只能保留PTE_V，
   * 不能保留R/W/X/U等叶子权限。
   */
  *superpte =
    PA2PTE((uint64)newpt) |
    PTE_V;

  /*
   * 页表发生变化，清除可能存在的旧TLB项。
   */
  sfence_vma();

  /*
   * 新的普通页已经保存了全部数据，
   * 因此可以释放原来的2 MiB超级页。
   */
  superfree((void *)oldpa);

  return 0;

err:
  /*
   * 分配中途失败时，释放此前已经
   * 分配成功的普通页。
   */
  for(int j = 0; j < i; j++){
    if(newpt[j] & PTE_V){
      uint64 pa =
        PTE2PA(newpt[j]);

      kfree((void *)pa);
      newpt[j] = 0;
    }
  }

  /*
   * 释放作为L0页表使用的物理页。
   */
  kfree((void *)newpt);

  /*
   * 原L1超级页PTE尚未被修改，
   * 因此原映射仍然有效，不需要回滚。
   */
  return -1;
}
// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable,uint64 va,uint64 npages,int do_free)
{
  uint64 a;
  uint64 end;

  /*
   * uvmunmap的起始虚拟地址至少需要按
   * 普通4 KiB页面对齐。
   */
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  if(va >= MAXVA)
    panic("uvmunmap: va too large");

  /*
   * 没有页面需要解除映射时直接返回。
   */
  if(npages == 0)
    return;

  /*
   * 防止va + npages * PGSIZE发生溢出，
   * 同时保证结束地址不超过MAXVA。
   */
  if(npages > (MAXVA - va) / PGSIZE)
    panic("uvmunmap: range too large");

  end = va + npages * PGSIZE;
  a = va;

  while(a < end){
    pte_t *pte;
    int level;

    /*
     * walkleaf能够识别：
     * level=0：4 KiB普通页叶子
     * level=1：2 MiB超级页叶子
     */
    pte = walkleaf(pagetable,
                   a,
                   &level);

    if(pte == 0 || (*pte & PTE_V) == 0){
      a += PGSIZE;
      continue;
    }

    /*
     * 处理第1级的2 MiB超级页叶子PTE。
     */
    if(level == 1){
      uint64 superbase;
      uint64 superend;

      superbase =
        SUPERPGROUNDDOWN(a);

      superend =
        superbase + SUPERPGSIZE;

      /*
       * 只有当前释放地址正好位于超级页起点，
       * 并且待释放范围完整覆盖整个超级页，
       * 才能直接解除超级页映射。
       */
      if(a == superbase &&
         end >= superend){
        if(do_free){
          uint64 pa =
            PTE2PA(*pte);

          superfree((void *)pa);
        }

        /*
         * 清除第1级超级页叶子PTE。
         */
        *pte = 0;

        /*
         * 一次跳过整个2 MiB超级页。
         */
        a += SUPERPGSIZE;
        continue;
      }

      /*
       * 当前待释放范围只覆盖超级页的一部分。
       *
       * 例如：
       * sbrk(-PGSIZE)
       *
       * 不能直接释放整个超级页，否则会破坏
       * 仍然属于进程的其余内存。
       *
       * 因此先把超级页降级成512个普通页。
       */
      if(superdemote(pagetable,
                     a) < 0)
        panic("uvmunmap: demote failed");

      /*
       * 降级完成后不要增加a。
       *
       * 下一轮重新查询相同虚拟地址，
       * 此时walkleaf()应该返回level=0的
       * 普通4 KiB页面PTE。
       */
      continue;
    }

   
    if(level != 0)
      panic("uvmunmap: unsupported leaf level");

    /*
     * 第0级有效PTE还必须是叶子PTE。
     * 如果只有PTE_V而没有R/W/X，则它不是
     * 有效的普通页面映射。
     */
    if((*pte &
        (PTE_R | PTE_W | PTE_X)) == 0)
      panic("uvmunmap: not a leaf");

    /*
     * do_free非0时，释放普通4 KiB物理页。
     */
    if(do_free){
      uint64 pa =
        PTE2PA(*pte);

      kfree((void *)pa);
    }

    /*
     * 清除第0级普通页叶子PTE。
     */
    *pte = 0;

    /*
     * 普通页按4 KiB向后移动。
     */
    a += PGSIZE;
  }
}

static int
allocnormalpage(pagetable_t pagetable,uint64 va, int perm)
{
  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);
  if(mappages(pagetable,va,PGSIZE,(uint64)mem,perm) != 0){
    kfree(mem);
    return -1;
  }

  return 0;
}
// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable,uint64 oldsz,uint64 newsz,int xperm)
{
  uint64 a;
  int perm = PTE_R | PTE_U | xperm;
  if(newsz < oldsz)
    return oldsz;
  oldsz = PGROUNDUP(oldsz);
  a = oldsz;
  // 第一段：分配到下一个2 MiB对齐地址
  while(a < newsz &&(a % SUPERPGSIZE) != 0){
    if(allocnormalpage(pagetable,a,perm) < 0)
      goto err;
    a += PGSIZE;
  }
  // 第二段：分配完整超级页
  while(a + SUPERPGSIZE <= newsz){
    char *mem = superalloc();
    if(mem == 0)
      break;
    if(supermappages(pagetable,a,(uint64)mem,perm) < 0){
      superfree(mem);
      goto err;
    }
    a += SUPERPGSIZE;
  }
  // 第三段：分配剩余普通页
  while(a < newsz){
    if(allocnormalpage(pagetable,a, perm) < 0)
      goto err;

    a += PGSIZE;
  }

  return newsz;

err:
  uvmdealloc(pagetable, a, oldsz);
  return 0;
}
// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 i = 0;
while(i < sz){
  int level;
  pte_t *pte =
    walkleaf(old, i, &level);

  if(pte == 0 ||
     (*pte & PTE_V) == 0){
    i += PGSIZE;
    continue;
  }

  uint64 pa = PTE2PA(*pte);
  uint flags = PTE_FLAGS(*pte);

  if(level == 1){
    char *mem = superalloc();

    if(mem == 0)
      goto err;

    memmove(mem,(char *)pa,SUPERPGSIZE);

    if(supermappages(new,
                     i,
                     (uint64)mem,
                     flags & ~PTE_V) < 0){
      superfree(mem);
      goto err;
    }

    i += SUPERPGSIZE;
  } else if(level == 0) {
    char *mem = kalloc();
    if(mem == 0)
      goto err;
    memmove(mem,(char *)pa,PGSIZE);
    if(mappages(new,i,PGSIZE,(uint64)mem,flags & ~PTE_V) < 0){
      kfree(mem);
      goto err;
    }

    i += PGSIZE;
  } else {
    panic("uvmcopy: unsupported leaf level");
  }
}

return 0;

err:
uvmunmap(new, 0, i / PGSIZE, 1);
return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist %lx %ld\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}




// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
