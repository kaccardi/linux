// SPDX-License-Identifier: GPL-2.0
/*
 * misc.c
 *
 * This is a collection of several routines used to extract the kernel
 * which includes KASLR relocation, decompression, ELF parsing, and
 * relocation processing. Additionally included are the screen and serial
 * output functions and related debugging support functions.
 *
 * malloc by Hannu Savolainen 1993 and Matthias Urlichs 1994
 * puts by Nick Holloway 1993, better puts by Martin Mares 1995
 * High loaded stuff by Hans Lermen & Werner Almesberger, Feb. 1996
 */

#include "misc.h"
#include "error.h"
#include "pgtable.h"
#include "../string.h"
#include "../voffset.h"

/*
 * WARNING!!
 * This code is compiled with -fPIC and it is relocated dynamically at
 * run time, but no relocation processing is performed. This means that
 * it is not safe to place pointers in static structures.
 */

/* Macros used by the included decompressor code below. */
#define STATIC		static

/*
 * Use normal definitions of mem*() from string.c. There are already
 * included header files which expect a definition of memset() and by
 * the time we define memset macro, it is too late.
 */
#undef memcpy
#undef memset
#define memzero(s, n)	memset((s), 0, (n))
#define memmove		memmove

/* Functions used by the included decompressor code below. */
void *memmove(void *dest, const void *src, size_t n);

/*
 * This is set up by the setup-routine at boot-time
 */
struct boot_params *boot_params;

memptr free_mem_ptr;
memptr free_mem_end_ptr;

static char *vidmem;
static int vidport;
static int lines, cols;

#ifdef CONFIG_KERNEL_GZIP
#include "../../../../lib/decompress_inflate.c"
#endif

#ifdef CONFIG_KERNEL_BZIP2
#include "../../../../lib/decompress_bunzip2.c"
#endif

#ifdef CONFIG_KERNEL_LZMA
#include "../../../../lib/decompress_unlzma.c"
#endif

#ifdef CONFIG_KERNEL_XZ
#include "../../../../lib/decompress_unxz.c"
#endif

#ifdef CONFIG_KERNEL_LZO
#include "../../../../lib/decompress_unlzo.c"
#endif

#ifdef CONFIG_KERNEL_LZ4
#include "../../../../lib/decompress_unlz4.c"
#endif

static unsigned long percpu_start;
static unsigned long percpu_end;

/*
 * NOTE: When adding a new decompressor, please update the analysis in
 * ../header.S.
 */

static void scroll(void)
{
	int i;

	memmove(vidmem, vidmem + cols * 2, (lines - 1) * cols * 2);
	for (i = (lines - 1) * cols * 2; i < lines * cols * 2; i += 2)
		vidmem[i] = ' ';
}

#define XMTRDY          0x20

#define TXR             0       /*  Transmit register (WRITE) */
#define LSR             5       /*  Line Status               */
static void serial_putchar(int ch)
{
	unsigned timeout = 0xffff;

	while ((inb(early_serial_base + LSR) & XMTRDY) == 0 && --timeout)
		cpu_relax();

	outb(ch, early_serial_base + TXR);
}

void __putstr(const char *s)
{
	int x, y, pos;
	char c;

	if (early_serial_base) {
		const char *str = s;
		while (*str) {
			if (*str == '\n')
				serial_putchar('\r');
			serial_putchar(*str++);
		}
	}

	if (lines == 0 || cols == 0)
		return;

	x = boot_params->screen_info.orig_x;
	y = boot_params->screen_info.orig_y;

	while ((c = *s++) != '\0') {
		if (c == '\n') {
			x = 0;
			if (++y >= lines) {
				scroll();
				y--;
			}
		} else {
			vidmem[(x + cols * y) * 2] = c;
			if (++x >= cols) {
				x = 0;
				if (++y >= lines) {
					scroll();
					y--;
				}
			}
		}
	}

	boot_params->screen_info.orig_x = x;
	boot_params->screen_info.orig_y = y;

	pos = (x + cols * y) * 2;	/* Update cursor position */
	outb(14, vidport);
	outb(0xff & (pos >> 9), vidport+1);
	outb(15, vidport);
	outb(0xff & (pos >> 1), vidport+1);
}

void __puthex(unsigned long value)
{
	char alpha[2] = "0";
	int bits;

	for (bits = sizeof(value) * 8 - 4; bits >= 0; bits -= 4) {
		unsigned long digit = (value >> bits) & 0xf;

		if (digit < 0xA)
			alpha[0] = '0' + digit;
		else
			alpha[0] = 'a' + (digit - 0xA);

		__putstr(alpha);
	}
}

static bool is_percpu_addr(long pc, long offset)
{
	unsigned long ptr;
	long address;

	address = pc + offset + 4;

	ptr = (unsigned long) address;

	if (ptr >= percpu_start && ptr < percpu_end)
		return true;

	return false;
}

/* called with unmodified address */
static bool address_in_section(long address, Elf64_Shdr *s)
{
	unsigned long ptr;
	unsigned long end = s->sh_addr + s->sh_size;

	ptr = (unsigned long) address;

	if (ptr >= s->sh_addr && ptr < end)
		return true;

	return false;
}

static Elf64_Shdr * adjust_address(long *address, Elf64_Shdr **sections)
{
	int i = 0;
	Elf64_Shdr *s;
	unsigned long offset;
	unsigned long old;

	if (sections == NULL) {
		debug_putstr("\nsections is null\n");
		return NULL;
	}

	s = sections[i++];
	while (s != NULL) {
		if (address_in_section(*address, s)) {
			*address += s->sh_offset;
			return s;
		}
		s = sections[i++];
	}
	return NULL;
}

static void adjust_relative_offset(long pc, long *value, Elf64_Shdr *section, Elf64_Shdr **sections)
{
	Elf64_Shdr *s;
	long address;

	/*
	 * Calculate the address that this offset would call.
	 */
	address = pc + *value + 4;

	/*
	 * if the address is in section that was randomized,
	 * we need to adjust the offset.
	 */
	s = adjust_address(&address, sections);
	if (s != NULL)
		*value = address - pc - 4;

	/*
	 * If the PC that this offset was calculated for was in a section
	 * that has been randomized, the value needs to be adjusted by the
	 * same amount as the randomized section was adjusted from it's original
	 * location.
	 */
	if (section != NULL)
		*value -= section->sh_offset;

}

#if CONFIG_X86_NEED_RELOCS
/*
 * TBD: find a way to get rid of sections or else find a way to make it
 * build for other than x86_64.
 */
static void handle_relocations(void *output, unsigned long output_len,
			       unsigned long virt_addr, Elf64_Shdr **sections)
{
	int *reloc;
	unsigned long delta, map, ptr;
	unsigned long min_addr = (unsigned long)output;
	unsigned long max_addr = min_addr + (VO___bss_start - VO__text);

	/*
	 * Calculate the delta between where vmlinux was linked to load
	 * and where it was actually loaded.
	 */
	delta = min_addr - LOAD_PHYSICAL_ADDR;

	/*
	 * The kernel contains a table of relocation addresses. Those
	 * addresses have the final load address of the kernel in virtual
	 * memory. We are currently working in the self map. So we need to
	 * create an adjustment for kernel memory addresses to the self map.
	 * This will involve subtracting out the base address of the kernel.
	 */
	map = delta - __START_KERNEL_map;

	/*
	 * 32-bit always performs relocations. 64-bit relocations are only
	 * needed if KASLR has chosen a different starting address offset
	 * from __START_KERNEL_map.
	 */
	if (IS_ENABLED(CONFIG_X86_64))
		delta = virt_addr - LOAD_PHYSICAL_ADDR;

	if (!delta) {
		debug_putstr("No relocation needed... ");
		return;
	}
	debug_putstr("\nPerforming relocations... ");

	/*
	 * Process relocations: 32 bit relocations first then 64 bit after.
	 * Three sets of binary relocations are added to the end of the kernel
	 * before compression. Each relocation table entry is the kernel
	 * address of the location which needs to be updated stored as a
	 * 32-bit value which is sign extended to 64 bits.
	 *
	 * Format is:
	 *
	 * kernel bits...
	 * 0 - zero terminator for 64 bit relocations
	 * 64 bit relocation repeated
	 * 0 - zero terminator for inverse 32 bit relocations
	 * 32 bit inverse relocation repeated
	 * 0 - zero terminator for 32 bit relocations
	 * 32 bit relocation repeated
	 *
	 * So we work backwards from the end of the decompressed image.
	 */
	for (reloc = output + output_len - sizeof(*reloc); *reloc; reloc--) {
		long extended = *reloc;
		long value;

		(void) adjust_address(&extended, sections);

		extended += map;

		ptr = (unsigned long)extended;
		if (ptr < min_addr || ptr > max_addr)
			error("32-bit relocation outside of kernel!\n");

		value = *(int32_t *)ptr;

		adjust_address(&value, sections);

		value += delta;

		*(uint32_t *)ptr = value;
	}
#ifdef CONFIG_X86_64
	while (*--reloc) {
		long extended = *reloc;
		long value;
		long oldvalue;
		Elf64_Shdr *s;

		s = adjust_address(&extended, sections);

		extended += map;

		ptr = (unsigned long)extended;
		if (ptr < min_addr || ptr > max_addr)
			error("inverse 32-bit relocation outside of kernel!\n");

		value = *(int32_t *)ptr;
		oldvalue = value;

		adjust_relative_offset(*reloc, &value, s, sections);

		/*
		 * only percpu symbols need to have their values adjusted for kaslr
		 * since relative offsets within the .text and .text.* sections
		 * are ok wrt each other.
		 */
		if (is_percpu_addr(*reloc, oldvalue)) {
			value -= delta;
		}

		*(int32_t *)ptr = value;
	}
	for (reloc--; *reloc; reloc--) {
		long extended = *reloc;
		long value;

		(void) adjust_address(&extended, sections);

		extended += map;

		ptr = (unsigned long)extended;
		if (ptr < min_addr || ptr > max_addr)
			error("64-bit relocation outside of kernel!\n");

		value = *(int64_t *)ptr;

		(void) adjust_address(&value, sections);

		value += delta;

		*(uint64_t *)ptr = value;
	}
#endif
}
#else
static inline void handle_relocations(void *output, unsigned long output_len,
				      unsigned long virt_addr)
{ }
#endif

static void shuffle_sections(Elf64_Shdr **list, int size)
{
	int i;
	unsigned long j;
	Elf64_Shdr *temp;

	for (i = size - 1; i > 0; i--) {
		/*
		 * TBD - seed. We need to be able to use a known
		 * seed so that we can non-randomly randomize for
		 * debugging.
		 */

		// pick a random index from 0 to i
		j = kaslr_get_random_long(NULL) % (i + 1);

		temp = list[i];
		list[i] = list[j];
		list[j] = temp;
	}
}

static void move_text(Elf64_Shdr **sections, int num_sections, char *secstrings, Elf64_Shdr *text, int rand_text_size, void *output, void *dest, Elf64_Phdr *phdr)
{
	void *fakeout;
	unsigned long adjusted_addr;
	int j;
	int left_bytes;
	void *offset;
	int adjusted_offset;
	const char *sname;

	memmove(dest, output + text->sh_offset, text->sh_size);
	fakeout = dest + text->sh_size;
	adjusted_addr = text->sh_addr + text->sh_size;

	shuffle_sections(sections, num_sections);

	/* now we'd walk through the sections. */
	for (j = 0; j < num_sections; j++) {
		Elf64_Shdr *s = sections[j];
		sname = secstrings + s->sh_name;
		debug_putstr("\n");
		debug_putstr(sname);
		debug_putstr(":orig addr ");
		debug_puthex(s->sh_addr);

		adjusted_offset = adjusted_addr - s->sh_addr;

		debug_putstr(" new addr: ");
		debug_puthex(s->sh_addr + adjusted_offset);

		memmove(fakeout, output + s->sh_offset, s->sh_size);
		fakeout += s->sh_size;
		adjusted_addr += s->sh_size;

		/* we can blow away sh_offset for our own uses */
		s->sh_offset = adjusted_offset;
	}
	left_bytes = phdr->p_filesz - text->sh_size - rand_text_size;
	memmove(fakeout, output + phdr->p_offset + text->sh_size + rand_text_size, left_bytes);
}

/*
 * TBD: make this build for other than x86_64
 */
static Elf64_Shdr ** parse_elf(void *output)
{
#ifdef CONFIG_X86_64
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdrs, *phdr;
#else
	Elf32_Ehdr ehdr;
	Elf32_Phdr *phdrs, *phdr;
#endif
	void *dest;
	int i;

	Elf64_Shdr *sechdrs;
	Elf64_Shdr *s;
	Elf64_Shdr *text = NULL;
	Elf64_Shdr *percpu = NULL;
	char *secstrings;
	int rand_text_size = 0;
	const char *sname;
	Elf64_Shdr **sections = NULL;
	int num_sections = 0;

	memcpy(&ehdr, output, sizeof(ehdr));
	if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
	   ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
	   ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
	   ehdr.e_ident[EI_MAG3] != ELFMAG3) {
		error("Kernel is not a valid ELF file");
		return NULL;
	}

	debug_putstr("Parsing ELF... ");

	/* we are going to need to allocate space for the section headers */
	sechdrs = malloc(sizeof(*sechdrs) * ehdr.e_shnum);
	if (!sechdrs)
		error("Failed to allocate space for shdrs");

	sections = malloc(sizeof(*sections) * ehdr.e_shnum);
	if (!sections)
		error("Failed to allocate space for section pointers");

	memcpy(sechdrs, output + ehdr.e_shoff, sizeof(*sechdrs) * ehdr.e_shnum);

	/* we need to allocate space for the section string table */
	s = &sechdrs[ehdr.e_shstrndx];

	secstrings = malloc(s->sh_size);
	if (!secstrings)
		error("Failed to allocate space for shstr");

	memcpy(secstrings, output + s->sh_offset, s->sh_size);

	/*
	 * now we need to walk through the section headers and collect the
	 * sizes of the .text sections to be randomized.
	 */
	for (i = 0; i < ehdr.e_shnum; i++) {
		s = &sechdrs[i];
		sname = secstrings + s->sh_name;

		if (!strcmp(sname, ".text")) {
			text = s;
			continue;
		}

		if (!strcmp(sname, ".data..percpu")) {
			/* get start addr for later */
			percpu = s;
		}

		if (!(s->sh_flags & SHF_ALLOC) ||
		    !(s->sh_flags & SHF_EXECINSTR) ||
		    !(strstarts(sname, ".text")))
			continue;

		rand_text_size += s->sh_size;
		sections[num_sections] = s;
		num_sections++;
	}
	sections[num_sections] = NULL;

	phdrs = malloc(sizeof(*phdrs) * ehdr.e_phnum);
	if (!phdrs)
		error("Failed to allocate space for phdrs");

	memcpy(phdrs, output + ehdr.e_phoff, sizeof(*phdrs) * ehdr.e_phnum);

	for (i = 0; i < ehdr.e_phnum; i++) {
		phdr = &phdrs[i];

		switch (phdr->p_type) {
		case PT_LOAD:
#ifdef CONFIG_X86_64
			if ((phdr->p_align % 0x200000) != 0)
				error("Alignment of LOAD segment isn't multiple of 2MB");
#endif
#ifdef CONFIG_RELOCATABLE
			dest = output;
			dest += (phdr->p_paddr - LOAD_PHYSICAL_ADDR);
#else
			dest = (void *)(phdr->p_paddr);
#endif
			if (text && (phdr->p_offset == text->sh_offset)) {
				move_text(sections, num_sections, secstrings,
					  text, rand_text_size, output, dest,
					  phdr);
			} else {
				if (percpu && (phdr->p_offset == percpu->sh_offset)) {
					percpu_start = percpu->sh_addr;
					percpu_end = percpu_start + phdr->p_filesz;
				}
				memmove(dest, output + phdr->p_offset,
					phdr->p_filesz);
			}
			break;
		default: /* Ignore other PT_* */ break;
		}
	}

	/* we need to keep the section info to redo relocs */
	free(secstrings);
	free(sechdrs);
	free(phdrs);
	return sections;
}

/*
 * The compressed kernel image (ZO), has been moved so that its position
 * is against the end of the buffer used to hold the uncompressed kernel
 * image (VO) and the execution environment (.bss, .brk), which makes sure
 * there is room to do the in-place decompression. (See header.S for the
 * calculations.)
 *
 *                             |-----compressed kernel image------|
 *                             V                                  V
 * 0                       extract_offset                      +INIT_SIZE
 * |-----------|---------------|-------------------------|--------|
 *             |               |                         |        |
 *           VO__text      startup_32 of ZO          VO__end    ZO__end
 *             ^                                         ^
 *             |-------uncompressed kernel image---------|
 *
 */
asmlinkage __visible void *extract_kernel(void *rmode, memptr heap,
				  unsigned char *input_data,
				  unsigned long input_len,
				  unsigned char *output,
				  unsigned long output_len)
{
	const unsigned long kernel_total_size = VO__end - VO__text;
	unsigned long virt_addr = LOAD_PHYSICAL_ADDR;
	Elf64_Shdr **sections;

	/* Retain x86 boot parameters pointer passed from startup_32/64. */
	boot_params = rmode;

	/* Clear flags intended for solely in-kernel use. */
	boot_params->hdr.loadflags &= ~KASLR_FLAG;

	/* Save RSDP address for later use. */
	/* boot_params->acpi_rsdp_addr = get_rsdp_addr(); */

	sanitize_boot_params(boot_params);

	if (boot_params->screen_info.orig_video_mode == 7) {
		vidmem = (char *) 0xb0000;
		vidport = 0x3b4;
	} else {
		vidmem = (char *) 0xb8000;
		vidport = 0x3d4;
	}

	lines = boot_params->screen_info.orig_video_lines;
	cols = boot_params->screen_info.orig_video_cols;

	console_init();
	debug_putstr("early console in extract_kernel\n");

	free_mem_ptr     = heap;	/* Heap */
	free_mem_end_ptr = heap + BOOT_HEAP_SIZE;

	/* Report initial kernel position details. */
	debug_putaddr(input_data);
	debug_putaddr(input_len);
	debug_putaddr(output);
	debug_putaddr(output_len);
	debug_putaddr(kernel_total_size);

#ifdef CONFIG_X86_64
	/* Report address of 32-bit trampoline */
	debug_putaddr(trampoline_32bit);
#endif

	/*
	 * The memory hole needed for the kernel is the larger of either
	 * the entire decompressed kernel plus relocation table, or the
	 * entire decompressed kernel plus .bss and .brk sections.
	 */
	choose_random_location((unsigned long)input_data, input_len,
				(unsigned long *)&output,
				max(output_len, kernel_total_size),
				&virt_addr);

	/* Validate memory location choices. */
	if ((unsigned long)output & (MIN_KERNEL_ALIGN - 1))
		error("Destination physical address inappropriately aligned");
	if (virt_addr & (MIN_KERNEL_ALIGN - 1))
		error("Destination virtual address inappropriately aligned");
#ifdef CONFIG_X86_64
	if (heap > 0x3fffffffffffUL)
		error("Destination address too large");
	if (virt_addr + max(output_len, kernel_total_size) > KERNEL_IMAGE_SIZE)
		error("Destination virtual address is beyond the kernel mapping area");
#else
	if (heap > ((-__PAGE_OFFSET-(128<<20)-1) & 0x7fffffff))
		error("Destination address too large");
#endif
#ifndef CONFIG_RELOCATABLE
	if ((unsigned long)output != LOAD_PHYSICAL_ADDR)
		error("Destination address does not match LOAD_PHYSICAL_ADDR");
	if (virt_addr != LOAD_PHYSICAL_ADDR)
		error("Destination virtual address changed when not relocatable");
#endif

	debug_putstr("\nDecompressing Linux... ");
	__decompress(input_data, input_len, NULL, NULL, output, output_len,
			NULL, error);
	sections = parse_elf(output);
	handle_relocations(output, output_len, virt_addr, sections);
	free(sections);
	debug_putstr("done.\nBooting the kernel.\n");
	return output;
}

void fortify_panic(const char *name)
{
	error("detected buffer overflow");
}
