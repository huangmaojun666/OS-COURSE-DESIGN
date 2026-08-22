// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers.
// Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[];

// 空闲物理页使用页本身保存链表结点。
struct run {
  struct run *next;
};

// 每个CPU拥有一把独立的锁和一条空闲页链表。
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// 将页面加入指定CPU的空闲链表。
static void
kfree_cpu(void *pa, int id)
{
  struct run *r = (struct run *)pa;

  memset(pa, 1, PGSIZE);

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}

void
kinit(void)
{
  // 初始化每个CPU对应的内存分配器锁。
  // 实验要求锁名称必须以"kmem"开头。
  for(int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
    kmem[i].freelist = 0;
  }

  // 将初始空闲页均匀放入各CPU的链表。
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int id = 0;

  p = (char *)PGROUNDUP((uint64)pa_start);

  // 初始化时把空闲页均匀分配到所有CPU，避免其他CPU首次分配时
  // 同时争抢启动CPU的唯一空闲链表。
  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE){
    kfree_cpu(p, id);
    id = (id + 1) % NCPU;
  }
}

// 释放pa指向的一页物理内存。
void
kfree(void *pa)
{
  int id;

  // 检查地址是否页对齐、是否位于合法物理内存范围内。
  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("kfree");

  /*
   * cpuid()只有在中断关闭时才能安全使用。
   * 如果中断开启，进程可能被调度到另一个CPU，
   * 从而把页面加入错误CPU的空闲链表。
   */
  push_off();
  id = cpuid();

  kfree_cpu(pa, id);
  pop_off();
}

// 分配一页4096字节的物理内存。
// 成功时返回内核可使用的地址，失败时返回0。
void *
kalloc(void)
{
  struct run *r = 0;
  int id;

  /*
   * 从取得CPU编号开始，一直到使用完这个编号，
   * 中间都不能重新开启中断。
   */
  push_off();
  id = cpuid();

  // 首先尝试从当前CPU自己的链表中分配。
  acquire(&kmem[id].lock);

  r = kmem[id].freelist;
  if(r != 0)
    kmem[id].freelist = r->next;

  release(&kmem[id].lock);

  /*
   * 当前CPU没有空闲页时，从其他CPU批量窃取。
   * 一次获取多页可以降低之后的跨CPU锁竞争。
   */
  if(r == 0){
    for(int offset = 1; offset < NCPU; offset++){
      int other = (id + offset) % NCPU;
      int low = id < other ? id : other;
      int high = id < other ? other : id;

      /*
       * 同时锁住源链表和目标链表，使页面在两个链表之间原子迁移。
       * 所有CPU都按编号从小到大加锁，避免两个CPU相互窃取时死锁。
       */
      acquire(&kmem[low].lock);
      acquire(&kmem[high].lock);

      r = kmem[other].freelist;
      if(r != 0){
        // 整条链表的摘取和挂接都是O(1)，不会长时间持有远端锁。
        kmem[other].freelist = 0;
        kmem[id].freelist = r->next;
        r->next = 0;
      }

      release(&kmem[high].lock);
      release(&kmem[low].lock);

      if(r != 0)
        break;
    }
  }

  pop_off();

  // 使用特殊值填充新分配的页面，帮助发现未初始化内存。
  if(r != 0)
    memset((char *)r, 5, PGSIZE);

  return (void *)r;
}