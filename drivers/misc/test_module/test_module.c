#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kristen Carlson Accardi");
MODULE_DESCRIPTION("Test module for boot randomization");
MODULE_VERSION("0.01");

static struct workqueue_struct *test_module_wq;
static struct work_struct work;
static int counter;

static void __attribute__((optimize("O0"))) test_module_do_work(void)
{
	printk("%s\n", __FUNCTION__);
	counter++;
	msleep(10000);
}

static void test_module_wq_func(struct work_struct *w)
{
	test_module_do_work();
	queue_work(test_module_wq, w);
	return;
}

static int __init test_module_init(void)
{
	int ret;

	test_module_wq = create_workqueue("test_module_queue");
	if (test_module_wq != NULL) {
		INIT_WORK(&work, test_module_wq_func);
		ret = queue_work(test_module_wq, &work);
	}
	printk(KERN_INFO "test_module_init\n");
	return 0;
}

static void __exit test_module_exit(void)
{
	flush_workqueue(test_module_wq);
	destroy_workqueue(test_module_wq);
	printk(KERN_INFO "test_module_exit\n");
}

module_init(test_module_init);
module_exit(test_module_exit);
