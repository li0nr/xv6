#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

struct spinlock e1000_tx_lock;
struct spinlock e1000_rx_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  initlock(&e1000_tx_lock, "tx_e1000");
  initlock(&e1000_rx_lock, "rx_e1000");
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
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
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
e1000_transmit(struct mbuf *m)
{
  // printf("new send\n");
  acquire(&e1000_tx_lock);
  uint16 tx_tail = regs[E1000_TDT];

  struct tx_desc *curr_desc = &tx_ring[tx_tail];

  if ((curr_desc->status & E1000_TXD_STAT_DD) == 0) {//prev packet is till not processed return error.
    release(&e1000_lock);
    return -1;
  }
  struct mbuf *prev_buffer = tx_mbufs[tx_tail];
  if (prev_buffer)
    mbuffree(prev_buffer);

  //save mbuf to be freed on next iteration.
  tx_mbufs[tx_tail] = m;

  //append the mbuf the tx ring.
  curr_desc->addr = (uint64) m->head;
  curr_desc->length = m->len;
  curr_desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  regs[E1000_TDT] = (tx_tail + 1) % TX_RING_SIZE;
  release(&e1000_tx_lock);
  return 0;
}
//
// static void
// e1000_recv(void)
// {
  // while ((rx_ring[recv_tail].status & E1000_RXD_STAT_DD) != 0) {
  //   printf("\n handling tail %d \n", recv_tail);
  //
  //   int len = rx_ring[recv_tail].length;
  //
  //   mbufput(rx_mbufs[recv_tail], len);
  //
  //   rx_ring[recv_tail].status = 0; // clear bits else we have a race
  //   __sync_synchronize();
  //
  //
  //   net_rx(rx_mbufs[recv_tail]);
  //
  //   printf("\n finished tail %d \n", recv_tail);
  //   //since the thread flaged the status as 0 we can manipulate the ring[rec_tail] without lock?
  //   // rx_mbufs[recv_tail] = mbufalloc(0);
  //   rx_ring[recv_tail].addr = (uint64) local_mbuf->head;
  //
  //   regs[E1000_RDT] = recv_tail;
  //   recv_tail = (regs[E1000_RDT]+1) % RX_RING_SIZE;
  //   local_mbuf = mbufalloc(0);
  //
  //   // release(&e1000_rx_lock);
  //   // local_mbuf = mbufalloc(0);
  //   // // printf("allocating %p\n", local_mbuf);
  //   // if (local_mbuf == 0) {
  //   //   printf("cant allocate new buf, cant handle recv");
  //   //   return;
  //   // }
  //   // acquire(&e1000_rx_lock);
  // }
  //   release(&e1000_rx_lock);
//}


static void
e1000_recv(void) {
  struct mbuf *recived_buf = 0;
  //dont want to allocate under lock
  struct mbuf *local_buf = mbufalloc(0);
  if (local_buf == 0) {
    printf("no more memory to receive packets\n");
    return;
  }

  uint32 recv_tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

  while((rx_ring[recv_tail].status & E1000_RXD_STAT_DD)) {
    acquire(&e1000_rx_lock);
    recived_buf = rx_mbufs[recv_tail];
    mbufput(recived_buf, rx_ring[recv_tail].length);

    // refill a new mbuf
    rx_mbufs[recv_tail] = local_buf;
    rx_ring[recv_tail].addr = (uint64) rx_mbufs[recv_tail]->head;
    rx_ring[recv_tail].status = 0;

    regs[E1000_RDT] = recv_tail;
    __sync_synchronize(); //not sure if this is needed but maybe to sync all othe processes.
    release(&e1000_rx_lock);

    net_rx(recived_buf);
    local_buf = mbufalloc(0);
    if (local_buf == 0) {
      printf("no more memory to receive packets\n");
      return;
    }
    recv_tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
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
