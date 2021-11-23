#ifndef JOS_INC_ELF_H
#define JOS_INC_ELF_H

#define ELF_MAGIC 0x464C457FU	/* "\x7FELF" in little endian */

/**
 * ELF文件由4部分组成，分别是ELF头（ELF header）、程序头表（Program header table）、节（Section）和节头表（Section header table）
 * 实际上，一个文件中不一定包含全部内容，而且它们的位置也未必如同所示这样安排，只有ELF头的位置是固定的，其余各部分的位置、大小等信息由ELF头中的各项值来决定
 */
struct Elf {
	uint32_t e_magic;	// must equal ELF_MAGIC
	uint8_t e_elf[12];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry; // 程序的入口地址
	uint32_t e_phoff; // Program header table 在文件中的偏移量（以字节计数）
	uint32_t e_shoff; // Section header table 在文件中的偏移量（以字节计数）
	uint32_t e_flags;
	uint16_t e_ehsize; // ELF header大小（以字节计数）
	uint16_t e_phentsize; // Program header table中每一个条目的大小
	uint16_t e_phnum; // Program header table中有多少个条目
	uint16_t e_shentsize; // Section header table中的每一个条目的大小
	uint16_t e_shnum; // Section header table中有多少个条目
	uint16_t e_shstrndx; // 节名称的字符串是第几个节（从零开始计数）
};

/**
 * Program header描述的是一个段在文件中的位置、大小以及它被放进内存后所在的位置和大小
 */
struct Proghdr {
	uint32_t p_type;
	uint32_t p_offset; // 第一个字节在文件中的偏移
	uint32_t p_va; // 一个字节在内存中的虚拟地址
	uint32_t p_pa; // 在物理内存定位相关的系统中，此项是为物理地址保留
	uint32_t p_filesz; // 在文件中的长度
	uint32_t p_memsz; // 在内存中的长度
	uint32_t p_flags;
	uint32_t p_align;
};

struct Secthdr {
	uint32_t sh_name;
	uint32_t sh_type;
	uint32_t sh_flags;
	uint32_t sh_addr;
	uint32_t sh_offset;
	uint32_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint32_t sh_addralign;
	uint32_t sh_entsize;
};

// Values for Proghdr::p_type
#define ELF_PROG_LOAD		1

// Flag bits for Proghdr::p_flags
#define ELF_PROG_FLAG_EXEC	1
#define ELF_PROG_FLAG_WRITE	2
#define ELF_PROG_FLAG_READ	4

// Values for Secthdr::sh_type
#define ELF_SHT_NULL		0
#define ELF_SHT_PROGBITS	1
#define ELF_SHT_SYMTAB		2
#define ELF_SHT_STRTAB		3

// Values for Secthdr::sh_name
#define ELF_SHN_UNDEF		0

#endif /* !JOS_INC_ELF_H */
