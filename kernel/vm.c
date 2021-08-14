#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  //申请一个内核页表，直接映射的方式建立内核页表，虚拟地址与物理地址一样，映射一系列硬件
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}
void
ukvmmap(pagetable_t kernel_pagetable ,uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}
//初始化进程独有的内核页表
pagetable_t
proc_kvminit(void)
{
  int i;
  pagetable_t proc_kpagetable = uvmcreate();
  if (proc_kpagetable == 0) {
    return 0;
  }
  for(i = 1; i < 512; i++) {
    proc_kpagetable[i] = kernel_pagetable[i];
  }

  ukvmmap(proc_kpagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  ukvmmap(proc_kpagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  ukvmmap(proc_kpagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
  ukvmmap(proc_kpagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  return proc_kpagetable;
}


// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  //设置了SATP寄存器,告诉MMU来使用刚刚设置好的kernel_pagetable
  w_satp(MAKE_SATP(kernel_pagetable));
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
//   0..11 -- 12 bits of byte offset within the page.
//根据虚拟地址获取L0级页表中的页表项存储的物理地址。模拟了MMU,返回的是va对应的最低级page table的PTE
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  //虚拟地址va是否超过了39位虚拟地址构成的最大地址MAXVA
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    //Px获取va中对应级别页表的页表项下标，最终从pagetable取得页表项
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {//校验页表项是否有效
      pagetable = (pagetable_t)PTE2PA(*pte);//将页表项中物理页号（PPN）提取出来。
    } else {//页表项无效
        //alloc为1，申请一个物理page
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      //将pagetable填充4k个0
      memset(pagetable, 0, PGSIZE);

      //物理地址转化为页表项，并将校验位置1，赋值给页表项pte。
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  //返回L0页表地址中的pte项地址。
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
//用mappages把地址对应关系安装到kernel的页表
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//更新页表中的页表项，将从va开始的虚拟地址建立页表项，映射到从pa开始的物理地址，映射区间大小为size。
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    //物理地址转换为页表项，更新页表中的页表项。
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
//把页表里从虚拟地址va开始的n个page的对应关系删除(可选是否kfree物理地址)
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
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

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
//给定新老内存size。遍历老size到新size之间的页，每个页先kalloc分配物理内存，再mappages安装到页表。
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
//给定新老内存size，用uvmunmap删除其间的va对应关系(可选是否回收内存)。
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
//递归形式。用kfree回收整个页表本身。
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
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
//用uvmunmap删除页表中从0开始n个地址的对应关系(同时kfree每页的物理地址)。再freewalk删除页表本身。
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
//给定新老两个页表和size，把老页表0开始到size的虚拟地址的数据复制到新页表。
//从0开始的每一页虚拟地址，找到老表的PTE、物理地址，kalloc分配一页，
//把物理地址上的数据复制到新分配的页，再用mappages把虚拟地址和新分配的页安装到页表。
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
//标记一个最低级页表的页表项无效
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
//把数据从物理地址copy到虚拟地址对应的物理地址
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
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
// 把数据从虚拟地址对应的物理地址copy到物理地址
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  // uint64 n, va0, pa0;

  // while(len > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > len)
  //     n = len;
  //   memmove(dst, (void *)(pa0 + (srcva - va0)), n);

  //   len -= n;
  //   dst += n;
  //   srcva = va0 + PGSIZE;
  // }
  // return 0;
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  // uint64 n, va0, pa0;
  // int got_null = 0;

  // while(got_null == 0 && max > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > max)
  //     n = max;

  //   char *p = (char *) (pa0 + (srcva - va0));
  //   while(n > 0){
  //     if(*p == '\0'){
  //       *dst = '\0';
  //       got_null = 1;
  //       break;
  //     } else {
  //       *dst = *p;
  //     }
  //     --n;
  //     --max;
  //     p++;
  //     dst++;
  //   }

  //   srcva = va0 + PGSIZE;
  // }
  // if(got_null){
  //   return 0;
  // } else {
  //   return -1;
  // }
  return copyinstr_new(pagetable, dst, srcva, max);
}
void
_vmprint(pagetable_t pagetable, int level){
  //遍历页表pagetable中的512个页表项
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    //pte第一个标志位是Valid，位于第一位。如果Valid bit位为1，那么表明这是一条合法的PTE
    if ((pte & PTE_V))
    {
      for (int j = 0; j < level; j++)
      {
        if (j == 0)
        {
          printf("..");
        }
        else{
          printf("  ..");
        }
        
      }
      //pte右移10位，去掉标志位，左移12位设置偏移量为0，也就是下一级页表物理地址。
      uint64 child = PTE2PA(pte);
      //pte为页表项，页表项的结构是：63-54保留位，53-10物理页号（PPN）,9-0标志位。

      printf("%d: pte %p pa %p\n", i , pte, child);
      //最后一级页表的页表项会设置读、写、执行的标志位，如果是最后一级页表就不再递归打印
      if ((pte & (PTE_R|PTE_W|PTE_X)) == 0)
      _vmprint((pagetable_t)child, level + 1);
    }
    
  }
  
}
void
vmprint(pagetable_t pagetable){
  //SATP寄存器中存储的页表物理地址
  printf("page table %p\n",pagetable);
  _vmprint(pagetable,1);

}


// copy user page table to per process kernel page table
void
u2kvmcopy(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz) 
{
  uint64 va;
  pte_t *upte;
  pte_t *kpte;

  if(newsz >= PLIC)
    panic("u2kvmcopy: newsz too large");

  for (va = oldsz; va < newsz; va += PGSIZE) {
    upte = walk(pagetable, va, 0);
    kpte = walk(kpagetable, va, 1);
    *kpte = *upte;
    // because the user mapping in kernel page table is only used for copyin 
    // so the kernel don't need to have the W,X,U bit turned on
    *kpte &= ~(PTE_U|PTE_W|PTE_X);
  }
}