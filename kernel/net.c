#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define MAX_UDP_PORTS 16
#define UDP_QUEUE_SIZE 16
// 队列中的一个UDP数据包。
struct udp_packet {
  int src_ip;          // 源IP地址，主机字节序
  short src_port;      // 源端口，主机字节序
  char *payload;       // UDP有效载荷地址
  char *owner;         // 整个数据包缓冲区的起始地址
  int length;          // UDP有效载荷长度
};
// 一个已经绑定的UDP端口。
struct udp_port {
  int used;
  short port;
  int head;            // 下一个被recv取出的下标
  int tail;            // 下一个写入数据包的下标
  int count;           // 当前缓存的数据包数量
  struct udp_packet queue[UDP_QUEUE_SIZE];
};

static struct udp_port udp_ports[MAX_UDP_PORTS];

void
netinit(void)
{
  initlock(&netlock, "netlock");
  memset(udp_ports, 0, sizeof(udp_ports));
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port_arg;
  argint(0, &port_arg);
  // 系统调用参数最终按16位UDP端口保存。
  short port = (short)port_arg;
  acquire(&netlock);
  // 不允许同一个端口被重复绑定。
  for(int i = 0; i < MAX_UDP_PORTS; i++){
    if(udp_ports[i].used && udp_ports[i].port == port){
      release(&netlock);
      return -1;
    }
  }
  // 寻找一个空闲端口槽位。
  for(int i = 0; i < MAX_UDP_PORTS; i++){
    if(udp_ports[i].used == 0){
      udp_ports[i].used = 1;
      udp_ports[i].port = port;
      udp_ports[i].head = 0;
      udp_ports[i].tail = 0;
      udp_ports[i].count = 0;
      memset(udp_ports[i].queue, 0,sizeof(udp_ports[i].queue));
      release(&netlock);
      return 0;
    }
  }
  // 没有空闲槽位。
  release(&netlock);
  return -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  int dport_arg;
  uint64 src_addr;
  uint64 sport_addr;
  uint64 user_buf;
  int maxlen;

  argint(0, &dport_arg);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &user_buf);
  argint(4, &maxlen);

  if(maxlen < 0)
    return -1;

  short dport = (short)dport_arg;
  struct proc *p = myproc();

  acquire(&netlock);

  // 查找用户要接收的目标端口。
  struct udp_port *target = 0;

  for(int i = 0; i < MAX_UDP_PORTS; i++){
    if(udp_ports[i].used &&
       udp_ports[i].port == dport){
      target = &udp_ports[i];
      break;
    }
  }

  // recv之前必须先调用bind。
  if(target == 0){
    release(&netlock);
    return -1;
  }

  // 队列为空时阻塞，直到数据包到达。
  while(target->count == 0){
    if(killed(p)){
      release(&netlock);
      return -1;
    }

    sleep(target, &netlock);
  }

  // 取出最早到达的数据包。
  int index = target->head;
  struct udp_packet packet = target->queue[index];

  memset(&target->queue[index], 0,
         sizeof(target->queue[index]));

  target->head =
      (target->head + 1) % UDP_QUEUE_SIZE;
  target->count--;

  release(&netlock);

  // 最多复制maxlen字节。
  int copy_length = packet.length;
  if(copy_length > maxlen)
    copy_length = maxlen;

  // 将源IP地址复制到用户空间。
  if(copyout(p->pagetable,
             src_addr,
             (char *)&packet.src_ip,
             sizeof(packet.src_ip)) < 0){
    kfree(packet.owner);
    return -1;
  }

  // 将源UDP端口复制到用户空间。
  if(copyout(p->pagetable,
             sport_addr,
             (char *)&packet.src_port,
             sizeof(packet.src_port)) < 0){
    kfree(packet.owner);
    return -1;
  }

  // 将UDP有效载荷复制到用户缓冲区。
  if(copyout(p->pagetable,
             user_buf,
             packet.payload,
             copy_length) < 0){
    kfree(packet.owner);
    return -1;
  }

  // 数据已经复制到用户空间，释放内核数据包缓冲区。
  kfree(packet.owner);

  return copy_length;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // 不要删除该输出，make grade需要使用它。
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;
  // 至少应包含以太网、IP和UDP三个首部。
  if(len < sizeof(struct eth) +sizeof(struct ip) +sizeof(struct udp)){
    kfree(buf);
    return;
  }
  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);
  // 本实验只接收UDP数据包。
  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }
  struct udp *udp = (struct udp *)(ip + 1);
  // 网络中的多字节整数采用网络字节序。
  int src_ip = ntohl(ip->ip_src);
  short src_port = ntohs(udp->sport);
  short dst_port = ntohs(udp->dport);
  int udp_length = ntohs(udp->ulen);
  // UDP长度包括UDP首部，因此不能小于首部长度。
  if(udp_length < sizeof(struct udp)){
    kfree(buf);
    return;
  }
  // 防止UDP长度超过实际收到的数据包长度。
  int available = len - sizeof(struct eth) - sizeof(struct ip);
  if(udp_length > available){
    kfree(buf);
    return;
  }

  int payload_length = udp_length - sizeof(struct udp);
  char *payload = (char *)(udp + 1);

  acquire(&netlock);

  // 查找目标端口。
  struct udp_port *target = 0;

  for(int i = 0; i < MAX_UDP_PORTS; i++){
    if(udp_ports[i].used &&
       udp_ports[i].port == dst_port){
      target = &udp_ports[i];
      break;
    }
  }

  // 目标端口没有绑定，直接丢弃数据包。
  if(target == 0){
    release(&netlock);
    kfree(buf);
    return;
  }

  // 每个端口最多缓存16个数据包。
  if(target->count == UDP_QUEUE_SIZE){
    release(&netlock);
    kfree(buf);
    return;
  }

  // 将数据包放入目标端口的环形队列。
  int index = target->tail;

  target->queue[index].src_ip = src_ip;
  target->queue[index].src_port = src_port;
  target->queue[index].payload = payload;
  target->queue[index].owner = buf;
  target->queue[index].length = payload_length;

  target->tail =
      (target->tail + 1) % UDP_QUEUE_SIZE;
  target->count++;

  // 唤醒正在等待该端口数据包的recv进程。
  wakeup(target);

  release(&netlock);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
