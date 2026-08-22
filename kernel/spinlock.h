// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

#ifdef LAB_LOCK
// Reader-writer lock.
struct rwspinlock {
  // 只用于保护下面三个状态变量。
  // 它不会在整个读临界区或写临界区内一直持有。
  struct spinlock guard;

  // 当前持有读锁的读者数量。
  uint readers;

  // 是否有写者持有写锁，取值为0或1。
  uint writer_active;

  // 正在等待获得写锁的写者数量。
  uint waiting_writers;
};
#endif
