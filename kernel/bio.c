// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  struct spinlock hash_lock[31];
  struct buf hashmap[31]; // hash map :)

  struct spinlock list_lock;
  struct buf vacant_buff;

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  bcache.vacant_buff.prev= &bcache.vacant_buff;
  bcache.vacant_buff.next = &bcache.vacant_buff;
  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.vacant_buff.next;
    b->prev = &bcache.vacant_buff;
    initsleeplock(&b->lock, "buffer");
    bcache.vacant_buff.next->prev = b;
    bcache.vacant_buff.next = b;
  }

  for (int i=0; i <31; i++) {
    bcache.hashmap[i].valid = 0; //empty
    bcache.hashmap[i].next = &bcache.hashmap[i]; // with no next
    initlock(bcache.hash_lock+i, "bcache-hash-bucket-lock");
  }
  printf("done init buffer cache\n");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint id =  blockno % 31;
  // acquire(&bcache.lock);
  acquire(&bcache.hash_lock[id]);
  // printf("bget with id=%d\n", id);
  // Is the block already cached?
  for(b = bcache.hashmap[id].next; b != &bcache.hashmap[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      // release(&bcache.lock);
      release(&bcache.hash_lock[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // printf("starting cas\n");
  struct buf *empty_buff;
  do {
    empty_buff = bcache.vacant_buff.next;
    // printf("empty_buff=%p, memory-tocas=%p, new_mem=%p\n",
    //        bcache.vacant_buff.next, empty_buff, empty_buff->next);

  } while (!__sync_bool_compare_and_swap(&bcache.vacant_buff.next, empty_buff,
                                         empty_buff->next));
  // printf("done cas\n");

    if(empty_buff->refcnt == 0) {
      empty_buff->dev = dev;
      empty_buff->blockno = blockno;
      empty_buff->valid = 0;
      empty_buff->refcnt = 1;
    // insert to hash map
      empty_buff->next = bcache.hashmap[id].next;
      empty_buff->prev = &bcache.hashmap[id];
      bcache.hashmap[id].next->prev = empty_buff;
      bcache.hashmap[id].next = empty_buff;
      release(&bcache.hash_lock[id]);
      acquiresleep(&empty_buff->lock);
        // printf("done bget with id=%d", id);
      return empty_buff;
    } else {
    panic("got a used buffer?");
  }
    // printf("done bget with id=%d", id);

  // for(b = bcache.vacant_buff.next; b != &bcache.vacant_buff; b = b->next){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  uint id =  b->blockno % 31;
  // acquire(&bcache.lock);
  acquire(&bcache.hash_lock[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
    //return to vacant_buf list
    struct buf *empty_buff;
    do {
      empty_buff = bcache.vacant_buff.next;
      b->next = empty_buff;
      b->prev = &bcache.vacant_buff;
    } while (
        !__sync_bool_compare_and_swap(&bcache.vacant_buff.next, empty_buff, b));
  }

  release(&bcache.hash_lock[id]);
}

void
bpin(struct buf *b) {
  uint id =  b->blockno % 31;
  // acquire(&bcache.lock);
  acquire(&bcache.hash_lock[id]);
  b->refcnt++;
  release(&bcache.hash_lock[id]);
}

void
bunpin(struct buf *b) {
  uint id =  b->blockno % 31;
  // acquire(&bcache.lock);
  acquire(&bcache.hash_lock[id]);
  b->refcnt--;
  release(&bcache.hash_lock[id]);
}


