/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

struct Env *envs = NULL;		// All environments
static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)

#define ENVGENSHIFT	12		// >= LOGNENV

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
struct Segdesc gdt[NCPU + 5] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// Per-CPU TSS descriptors (starting from GD_TSS0) are initialized
	// in trap_init_percpu()
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
    // TODO:为什么要检查是否为旧env
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the env_free_list.
// Make sure the environments are in the free list in the same order
// they are in the envs array (i.e., so that the first call to
// env_alloc() returns envs[0]).
//
void
env_init(void) {
    // Set up envs array
    // LAB 3: Your code here.
    // 此处要与envs array中保持一致，这样每次allocate空env时使用env[0]
    for (int i = NENV - 1; i >= 0; --i) {
        envs[i].env_id = 0;
        envs[i].env_link = env_free_list;
        env_free_list = &envs[i];
    }

    // Per-CPU part of the initialization
    env_init_percpu();
}

// Load GDT and segment descriptors.
void
env_init_percpu(void) {
    lgdt(&gdt_pd);
    // The kernel never uses GS or FS, so we leave those set to
    // the user data segment.
    asm volatile("movw %%ax,%%gs" : : "a" (GD_UD | 3));
    asm volatile("movw %%ax,%%fs" : : "a" (GD_UD | 3));
    // The kernel does use ES, DS, and SS.  We'll change between
    // the kernel and user data segments as needed.
    asm volatile("movw %%ax,%%es" : : "a" (GD_KD));
    asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
    asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));
    // Load the kernel text segment into CS.
    asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT));
    // For good measure, clear the local descriptor table (LDT),
    // since we don't use it.
    lldt(0);
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page directory, set e->env_pgdir accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
/**
 * 初始化新的用户环境的页目录表
 * @param e
 * @return
 */
static int
env_setup_vm(struct Env *e) {
    int i;
    struct PageInfo *p = NULL;

    // Allocate a page for the page directory
    if (!(p = page_alloc(ALLOC_ZERO)))
        return -E_NO_MEM;

    // Now, set e->env_pgdir and initialize the page directory.
    //
    // Hint:
    //    - The VA space of all envs is identical above UTOP
    //	(except at UVPT, which we've set below).
    //	See inc/memlayout.h for permissions and layout.
    //	Can you use kern_pgdir as a template?  Hint: Yes.
    //	(Make sure you got the permissions right in Lab 2.)
    //    - The initial VA below UTOP is empty.
    //    - You do not need to make any more calls to page_alloc.
    //    - Note: In general, pp_ref is not maintained for
    //	physical pages mapped only above UTOP, but env_pgdir
    //	is an exception -- you need to increment env_pgdir's
    //	pp_ref for env_free to work correctly.
    //    - The functions in kern/pmap.h are handy.

    // 将分配的页交给env
    e->env_pgdir = page2kva(p);
    // 一般而言，物理地址在UTOP以上的pp_ref不用维护，此处为了保证env_pgdir的env_free正常运行
    p->pp_ref++;
    // UTOP: Top of user-accessible VM，上面才有用户VM
    // NPDENTRIES: pgdir总数
    for (i = PDX(UTOP); i < NPDENTRIES; i++) {
        // Can you use kern_pgdir as a template?  Hint: Yes.
        e->env_pgdir[i] = kern_pgdir[i];
    }

    // UVPT maps the env's own page table read-only.
    // Permissions: kernel R, user R
    e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;

    return 0;
}

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENV environments are allocated
//	-E_NO_MEM on memory exhaustion
//
int
env_alloc(struct Env **newenv_store, envid_t parent_id) {
    int32_t generation;
    int r;
    struct Env *e;

    if (!(e = env_free_list))
        return -E_NO_FREE_ENV;

    // Allocate and set up the page directory for this environment.
    if ((r = env_setup_vm(e)) < 0)
        return r;

    // Generate an env_id for this environment.
    generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    if (generation <= 0)    // Don't create a negative env_id.
        generation = 1 << ENVGENSHIFT;
    e->env_id = generation | (e - envs);

    // Set the basic status variables.
    e->env_parent_id = parent_id;
    e->env_type = ENV_TYPE_USER;
    e->env_status = ENV_RUNNABLE;
    e->env_runs = 0;

    // Clear out all the saved register state,
    // to prevent the register values
    // of a prior environment inhabiting this Env structure
    // from "leaking" into our new environment.
    memset(&e->env_tf, 0, sizeof(e->env_tf));

    // Set up appropriate initial values for the segment registers.
    // GD_UD is the user data segment selector in the GDT, and
    // GD_UT is the user text segment selector (see inc/memlayout.h).
    // The low 2 bits of each segment register contains the
    // Requestor Privilege Level (RPL); 3 means user mode.  When
    // we switch privilege levels, the hardware does various
    // checks involving the RPL and the Descriptor Privilege Level
    // (DPL) stored in the descriptors themselves.
    e->env_tf.tf_ds = GD_UD | 3;
    e->env_tf.tf_es = GD_UD | 3;
    e->env_tf.tf_ss = GD_UD | 3;
    e->env_tf.tf_esp = USTACKTOP;
    e->env_tf.tf_cs = GD_UT | 3;
    // You will set e->env_tf.tf_eip later.

    // commit the allocation
    env_free_list = e->env_link;
    *newenv_store = e;

    cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
    return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Does not zero or otherwise initialize the mapped pages in any way.
// Pages should be writable by user and kernel.
// Panic if any allocation attempt fails.
//
/**
 * 为env分配物理空间并映射
 * @param e env
 * @param va 映射地址
 * @param len
 */
static void
region_alloc(struct Env *e, void *va, size_t len) {
    // LAB 3: Your code here.
    // (But only if you need it for load_icode.)
    //
    // Hint: It is easier to use region_alloc if the caller can pass
    //   'va' and 'len' values that are not page-aligned.
    //   You should round va down, and round (va + len) up.
    //   (Watch out for corner-cases!)
    // 注意对齐
    void *begin = ROUNDDOWN(va, PGSIZE);
    void *end = ROUNDUP(va + len, PGSIZE);
    for (; begin < end; begin += PGSIZE) {
        struct PageInfo *new_page = page_alloc(ALLOC_ZERO);
        if (!new_page)
            panic("region_alloc failed!");
        page_insert(e->env_pgdir, new_page, begin, PTE_U | PTE_W);
    }
}

//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// All this is very similar to what our boot loader does, except the boot
// loader also needs to read the code from disk.  Take a look at
// boot/main.c to get ideas.
//
// Finally, this function maps one page for the program's initial stack.
//
// load_icode panics if it encounters problems.
//  - How might load_icode fail?  What might be wrong with the given input?
//
/**
 * TODO: 为什么要load一个ELF文件？
 * 首先要清楚JOS使用的ELF文件格式
 * 我们要判断ELF文件格式是否合法，并加载每一个被标记为ELF_PROG_LOAD的program header对应的segment
 * binary给出了ELF文件被嵌在内核中的位置
 * program header中声明了对应segment在ELF文件本身所占的大小p->filesz和在内存中将要占据的内存大小p->memsz，且前者不会大于后者，内存中剩余的部分我们要清零
 * 显然，我们要先通过region_alloc分配好物理内存，然后再将用户程序复制到目标区域
 * 这里的一个关键点是，我们将要把用户程序加载到的目标地址位于用户空间上，而当前使用的页目录是kern_pgdir，这上面并没有映射UTOP（UENVS）以下的地址空间
 * 为了能够加载，我们需要临时切换到region_alloc中已经为进程创建好的页目录
 * 数据复制完毕后，我们再将页目录还原
 * 除了用户程序，我们还要为进程分配一个大小为PGSIZE的用户栈，由USTACKTOP-PGSIZE指向对应物理页
 * 另外特别要注意的是，我们需要在此处设置好进程的trapframe中的eip，使得进程在启动的时候可以从正确的程序入口开始执行，否则后面的env_run就会出错
 * @param e
 * @param binary
 */
static void
load_icode(struct Env *e, uint8_t *binary) {
    // Hints:
    //  Load each program segment into virtual memory
    //  at the address specified in the ELF segment header.
    //  You should only load segments with ph->p_type == ELF_PROG_LOAD.
    //  Each segment's virtual address can be found in ph->p_va
    //  and its size in memory can be found in ph->p_memsz.
    //  The ph->p_filesz bytes from the ELF binary, starting at
    //  'binary + ph->p_offset', should be copied to virtual address
    //  ph->p_va.  Any remaining memory bytes should be cleared to zero.
    //  (The ELF header should have ph->p_filesz <= ph->p_memsz.)
    //  Use functions from the previous lab to allocate and map pages.
    //
    //  All page protection bits should be user read/write for now.
    //  ELF segments are not necessarily page-aligned, but you can
    //  assume for this function that no two segments will touch
    //  the same virtual page.
    //
    //  You may find a function like region_alloc useful.
    //
    //  Loading the segments is much simpler if you can move data
    //  directly into the virtual addresses stored in the ELF binary.
    //  So which page directory should be in force during
    //  this function?
    //
    //  You must also do something with the program's entry point,
    //  to make sure that the environment starts executing there.
    //  What?  (See env_run() and env_pop_tf() below.)

    struct Elf *ELFHDR = (struct Elf *) binary;
    struct Proghdr *ph, *eph;

    // must equal ELF_MAGIC
    if (ELFHDR->e_magic != ELF_MAGIC)
        panic("Not executable!");

    // 将eip指向entry
    e->env_tf.tf_eip = ELFHDR->e_entry;
    ph = (struct Proghdr *) ((uint8_t *) ELFHDR + ELFHDR->e_phoff);
    eph = ph + ELFHDR->e_phnum;

    // load user pgdir
    lcr3(PADDR(e->env_pgdir));

    // iterate all the items
    for (; ph < eph; ++ph) {
        // only load segments with ph->p_type == ELF_PROG_LOAD
        if (ph->p_type == ELF_PROG_LOAD) {
            // The ELF header should have ph->p_filesz <= ph->p_memsz
            // 文件中的长度不能超过内存中的长度
            if (ph->p_filesz > ph->p_memsz) {
                panic("load icode failed : p_memsz < p_filesz.\n");
            }
            // 分配一个内存大小的Program header，并分配物理页
            region_alloc(e, (void *) ph->p_va, ph->p_memsz);
            memset((void *) ph->p_va, 0, ph->p_memsz);
            //  The ph->p_filesz bytes from the ELF binary, starting at
            //  'binary + ph->p_offset', should be copied to virtual address
            //  ph->p_va.
            memcpy((void *) ph->p_va, binary + ph->p_offset, ph->p_filesz);
        }
    }

    // back to kern_pgdir
    lcr3(PADDR(kern_pgdir));

    // Now map one page for the program's initial stack
    // at virtual address USTACKTOP - PGSIZE.
    region_alloc(e, (void *) (USTACKTOP - PGSIZE), PGSIZE);
}

//
// Allocates a new env with env_alloc, loads the named elf
// binary into it with load_icode, and sets its env_type.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
//
/**
 * 创建env环境
 * @param binary
 * @param type
 */
void
env_create(uint8_t *binary, enum EnvType type) {
    struct Env *e;
    if (env_alloc(&e, 0) != 0) {
        panic("env_create failed: env_alloc failed.\n");
    }
    load_icode(e, binary);
    e->env_type = type;
}

//
// Frees env e and all memory it uses.
//
void
env_free(struct Env *e) {
    pte_t *pt;
    uint32_t pdeno, pteno;
    physaddr_t pa;

    // If freeing the current environment, switch to kern_pgdir
    // before freeing the page directory, just in case the page
    // gets reused.
    if (e == curenv)
        lcr3(PADDR(kern_pgdir));

    // Note the environment's demise.
    cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

    // Flush all mapped pages in the user portion of the address space
    static_assert(UTOP % PTSIZE == 0);
    for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

        // only look at mapped page tables
        if (!(e->env_pgdir[pdeno] & PTE_P))
            continue;

        // find the pa and va of the page table
        pa = PTE_ADDR(e->env_pgdir[pdeno]);
        pt = (pte_t *) KADDR(pa);

        // unmap all PTEs in this page table
        for (pteno = 0; pteno <= PTX(~0); pteno++) {
            if (pt[pteno] & PTE_P)
                page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
        }

        // free the page table itself
        e->env_pgdir[pdeno] = 0;
        page_decref(pa2page(pa));
    }

    // free the page directory
    pa = PADDR(e->env_pgdir);
    e->env_pgdir = 0;
    page_decref(pa2page(pa));

    // return the environment to the free list
    e->env_status = ENV_FREE;
    e->env_link = env_free_list;
    env_free_list = e;
}

//
// Frees environment e.
//
void
env_destroy(struct Env *e) {
    env_free(e);

    cprintf("Destroyed the only environment - nothing more to do!\n");
    while (1)
        monitor(NULL);
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
void
env_pop_tf(struct Trapframe *tf)
{
	// Record the CPU we are running on for user-space debugging
	curenv->env_cpunum = cpunum();

	asm volatile(
		"\tmovl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret\n"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

//
// Context switch from curenv to env e.
// Note: if this is the first call to env_run, curenv is NULL.
//
// This function does not return.
//
void
env_run(struct Env *e) {
    // Step 1: If this is a context switch (a new environment is running):
    //	   1. Set the current environment (if any) back to
    //	      ENV_RUNNABLE if it is ENV_RUNNING (think about
    //	      what other states it can be in)
    //	   2. Set 'curenv' to the new environment,
    //	   3. Set its status to ENV_RUNNING,
    //	   4. Update its 'env_runs' counter,
    //	   5. Use lcr3() to switch to its address space.
    if (curenv && curenv->env_status == ENV_RUNNING) {
        curenv->env_status = ENV_RUNNABLE;
    }
    curenv = e;
    curenv->env_status = ENV_RUNNING;
    curenv->env_runs++;
    lcr3(PADDR(curenv->env_pgdir));

    // In env_run(), release the lock right before switching to user mode.
    // Do not do that too early or too late, otherwise you will experience races or deadlocks.
    unlock_kernel();

    // Step 2: Use env_pop_tf() to restore the environment's
    //	   registers and drop into user mode in the
    //	   environment.
    env_pop_tf(&curenv->env_tf);
}

