// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "fgkaslr: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <asm/io.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kristen Carlson Accardi");
MODULE_DESCRIPTION("Test module for boot randomization");
MODULE_VERSION("0.01");

static struct workqueue_struct *test_module_wq;
static struct work_struct work;
static int counter;
DEFINE_PER_CPU(int, per_cpu_var);

static void __attribute__((optimize("O0"))) test_module_do_work(void)
{
	phys_addr_t phys;

	/*
	 * Because virt_to_phys is inline, this will create a
	 * reloc with a relative offset to a function section that has
	 * been randomized.
	 */
	phys = virt_to_phys(test_module_do_work);

	/*
	 * create reloc for relative offset to routine in non-randomized section
	 */
	pr_info("%s:%px phys:%llx\n", __func__, test_module_do_work, phys);

	/*
	 * create reloc which is relative offset to .bss
	 */
	counter++;

	/*
	 * create a reloc which references the per cpu section.
	 */
	this_cpu_inc(per_cpu_var);

	/*
	 * create reloc of type R_X86_64_PLT32
	 */
	msleep(10000);
}

static void __attribute__((optimize("O0"))) test_module_wq_func(struct work_struct *w)
{
	/*
	 * this call will create a reloc of type R_X86_64_32S
	 * to this function in a section that was randomized.
	 */
	test_module_do_work();

	/*
	 * Call to printk creates a reloc of type R_X86_64_PLT32,
	 * which is an offset relative to the program counter. We
	 * recalculate the new offset after we get our new random
	 * location at load time and replace the one the linker creates.
	 *
	 * This call also creates relocs of type R_X86_64_32S to the
	 * .rodata section, which requires just a direct address
	 * substitution for our new randomized location.
	 */
	pr_info("%s: enter\n", __func__);

	/*
	 * here we access .bss - and this creates a reloc of type R_X86_64_PC32
	 * which is an offset relative to the program counter - we handle this
	 * the same as the R_X86_64_PLT32 type.
	 */
	counter++;

	/*
	 * this is just a second R_X86_64_PLT32 entry
	 */
	msleep(10000);

	/*
	 * this is another R_X86_64_PLT32 entry - however, since queue_work()
	 * is an inline function, queue_work() will have it's own function
	 * section that will be randomized. That means our relative offset
	 * needs to be recalculated completely based on the new location of
	 * both queue_work() and test_module_wq_func(). This call also
	 * accesses .bss, which creates another R_X86_64_PC32 entry.
	 */
	queue_work(test_module_wq, w);
	return;
}

static int __init test_module_init(void)
{
	int ret;

	pr_info("%s\n", __func__);

	/*
	 * this call will create a reloc of type R_X86_64_PC32
	 * to this function from the .init section to a section
	 * that was randomized.
	 */
	test_module_do_work();

	/*
	 * here we are adding the address of a function that has
	 * been randomized. The reloc that was created should be
	 * updated to reflect the new address.
	 */
	test_module_wq = create_workqueue("test_module_queue");
	if (test_module_wq != NULL) {
		INIT_WORK(&work, test_module_wq_func);
		ret = queue_work(test_module_wq, &work);
	}

	return 0;
}

static void __exit test_module_exit(void)
{
	flush_workqueue(test_module_wq);
	destroy_workqueue(test_module_wq);
	pr_info("%s\n", __func__);
}

module_init(test_module_init);
module_exit(test_module_exit);
