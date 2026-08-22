#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
// this code loosely follows the initialization directions
// in Chapter 14 of Intel's Software Developer's Manual.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_ring[i].addr = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_ring[i].addr = (uint64) kalloc();
    if (!rx_ring[i].addr)
      panic("e1000");
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(char *buf, int len)
{
  uint32 idx;
  // 发送环可能被多个进程同时访问，因此需要加锁。
  acquire(&e1000_lock);
  // TDT 指向驱动程序下一次应填写的发送描述符。
  idx = regs[E1000_TDT];
  // DD 位没有置位，说明网卡还没有使用完该描述符。
  // 此时发送环已满，不能覆盖原来的数据。
  if((tx_ring[idx].status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    return -1;
  }
  // 当前描述符可能保存着上一次发送的数据缓冲区。
  // 只有 DD 置位后，才能说明网卡已经使用完该缓冲区。
  if(tx_ring[idx].addr != 0)
    kfree((void *)tx_ring[idx].addr);
  // 填写新的发送描述符。
  tx_ring[idx].addr = (uint64)buf;
  tx_ring[idx].length = len;
  // EOP 表示该描述符包含数据包的最后一段。
  // RS 要求网卡完成发送后写回描述符状态。
  tx_ring[idx].cmd =
      E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  // 将状态清零，将描述符交给网卡。
  tx_ring[idx].status = 0;
  // 更新发送环尾指针，并通知网卡出现了新的发送任务。
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  while(1){
    // RDT 指向驱动程序最后处理完的接收描述符。
    // 因此下一个可能包含新数据包的位置是 RDT + 1。
    uint32 idx =(regs[E1000_RDT] + 1) % RX_RING_SIZE;
    // DD 没有置位，说明网卡还没有在该描述符中写入数据包。
    if((rx_ring[idx].status & E1000_RXD_STAT_DD) == 0)
      return;
    // 保存数据包缓冲区地址和实际长度。
    char *buf = (char *)rx_ring[idx].addr;
    int len = rx_ring[idx].length;
    // 将数据包交给 xv6 网络协议栈处理。
    // net_rx 最终会释放 buf，因此驱动不能继续使用旧缓冲区。
    net_rx(buf, len);
    // 为当前描述符重新分配一页接收缓冲区。
    char *newbuf = kalloc();
    if(newbuf == 0)
      panic("e1000_recv: kalloc failed");
    rx_ring[idx].addr = (uint64)newbuf;
    // 清除旧状态，将描述符重新交给网卡。
    rx_ring[idx].status = 0;
    // 更新接收环尾指针，表示该描述符已经处理完成。
    regs[E1000_RDT] = idx;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
