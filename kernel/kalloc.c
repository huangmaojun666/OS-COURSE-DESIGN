// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct superrun {
  struct superrun *next;
};

struct {
  struct spinlock lock;
  struct superrun *freelist;
} supermem;
#define NSUPERPAGES 16
void
kinit()
{
  uint64 super_start;
  uint64 super_end;

  initlock(&kmem.lock, "kmem");
  initlock(&supermem.lock, "supermem");

  // 从内核结束地址之后的第一个2 MiB边界开始，
  // 预留NSUPERPAGES个连续的2 MiB区域。
  super_start =
    SUPERPGROUNDUP((uint64)end);

  super_end =
    super_start +
    NSUPERPAGES * SUPERPGSIZE;

  if(super_end > PHYSTOP)
    panic("kinit: not enough memory for superpages");

  // end到super_start之间可能存在不足2 MiB的对齐区域，
  // 这些物理页仍交给普通页分配器。
  freerange(end, (void *)super_start);

  // 将预留的2 MiB对齐区域加入超级页空闲链表。
  for(uint64 pa = super_start;
      pa < super_end;
      pa += SUPERPGSIZE){
    superfree((void *)pa);
  }

  // 超级页预留区域之后的物理内存，
  // 继续交给普通页分配器。
  freerange((void *)super_end,
            (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
void
superfree(void *pa)
{
  struct superrun *r;

  if(((uint64)pa % SUPERPGSIZE) != 0)
    panic("superfree");

  if((char *)pa < end ||
     (uint64)pa + SUPERPGSIZE > PHYSTOP)
    panic("superfree");

  memset(pa, 1, SUPERPGSIZE);

  r = (struct superrun *)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
void *
superalloc(void)
{
  struct superrun *r;

  acquire(&supermem.lock);
  r = supermem.freelist;

  if(r)
    supermem.freelist = r->next;

  release(&supermem.lock);

  if(r)
    memset((char *)r, 0, SUPERPGSIZE);

  return (void *)r;
}