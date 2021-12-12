// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
/**
 * 1. The kernel propagates the page fault to _pgfault_upcall, which calls fork()'s pgfault() handler.
   2. pgfault() checks that the fault is a write (check for FEC_WR in the error code)
   and that the PTE for the page is marked PTE_COW. If not, panic.
   3. pgfault() allocates a new page mapped at a temporary location and copies the contents of the faulting page into it.
   Then the fault handler maps the new page at the appropriate address with read/write permissions, in place of the old read-only mapping.
 * @param utf
 */
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
    if (!((err & FEC_WR) && (uvpt[PGNUM(addr)] & PTE_COW))) {
        panic("pgfault err");
    }

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
    if ( (r = sys_page_alloc(0, ROUNDDOWN(addr, PGSIZE), PTE_P|PTE_U|PTE_W) )< 0)
        panic("sys_page_alloc error: %d", r);
    memmove(addr, PFTEMP, PGSIZE);
    if ( (r = sys_page_map(0, ROUNDDOWN(addr, PGSIZE), 0, PFTEMP, PTE_P|PTE_U|PTE_W) )< 0)
         panic("sys_page_map error: %d", r);
    if ( (r = sys_page_unmap(0, PFTEMP) )< 0)
        panic("sys_page_map error: %d", r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
    // page offset
    int perm = PGOFF(uvpt[pn]);
    // 如果可写或写实复制的页
    if (perm & (PTE_W | PTE_COW)) {
        // 加上COW
        perm |= PTE_COW;
        // 去掉W
        perm &= ~PTE_W;
    }
    // 必须先设置子进程COW
    if ((r = sys_page_map(0, (void *) (pn * PGSIZE), envid, (void *) (pn * PGSIZE), perm)) < 0) {
        panic("duppage: %e", r);
    }
    // 父进程重新映射
    if ((r = sys_page_map(0, (void *) (pn * PGSIZE), 0, (void *) (pn * PGSIZE), perm)) < 0) {
        panic("duppage: %e", r);
    }
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void) {
    // LAB 4: Your code here.
    // 1. 使用set_pgfault_handler()设置缺页处理函数。
    set_pgfault_handler(pgfault);

    // 2. 调用sys_exofork()系统调用，在内核中创建一个Env结构，复制当前用户环境寄存器状态，UTOP以下的页目录还没有建立，新创建的进程还不能直接运行。
    envid_t envid = sys_exofork();
    if (envid < 0) {
        panic("error sys_exofork: %e", envid);
    } else if (envid == 0) {
        // 如果是子进程，修改当前env
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }

    // 3. 拷贝父进程的页表和页目录到子进程。对于可写的页，将对应的PTE的PTE_COW位设置为1。
    for (uint32_t i = 0; i < USTACKTOP; i += PGSIZE) {
        // pgd and pgt exist
        if ( (uvpd[PDX(i)] & PTE_P) && (uvpt[PGNUM(i)] & PTE_U)) {
            duppage(envid, PGNUM(i));
        }
    }

    // 4. 为子进程分配异常栈，设置_pgfault_upcall。
    int r;
    extern void _pgfault_upcall(void);
    if ((r = sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_P | PTE_W | PTE_U)) < 0)
        panic("sys_page_alloc: %e", r);
    sys_env_set_pgfault_upcall(envid, _pgfault_upcall);

    // 5. 将子进程状态设置为ENV_RUNNABLE。
    if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
        panic("sys_env_set_status: %e", r);

    return envid;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
