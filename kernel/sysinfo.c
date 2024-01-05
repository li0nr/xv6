
#include "sysinfo.h"

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"

uint64
fill_info(uint64 addr)
{
    struct sysinfo info;
    struct proc *p;
    p = myproc();

    info.freemem = avaliable_mem();
    info.nproc = avaliable_proc();

    if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
      return -1;
    return 0;
}

uint64
sys_info(void)
{
  uint64 st; // user pointer to struct stat

  argaddr(0, &st);
  return   fill_info(st);

}