#include "delay_free_inode.h"

static struct task_struct *thread;
static struct list_head block_list;
static struct kmem_cache* allocator; 
static spinlock_t iq_lock;
static unsigned long count_free_blocks;
static int thread_running;
static int thread_control;

static int kt_free_inode(void);

static struct kobject *frinode_kobj;

struct frinode_attr{
	struct kobj_attribute attr;
	int value;
};

static struct frinode_attr frinode_value;
static struct frinode_attr frinode_notify;

static struct attribute *frinode_attrs[] = {
	&frinode_value.attr.attr,
	&frinode_notify.attr.attr,
	NULL
};

static struct attribute_group frinode_group = {
	.attrs = frinode_attrs,
};

static ssize_t frinode_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "Number of free-ed inode: %lu\nThread running: %d\n", 
			count_free_blocks, thread_running);

}

static ssize_t frinode_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	struct frinode_attr *frinode = container_of(attr, struct frinode_attr, attr);
	int was_on = 0;
	if (frinode -> value)
		was_on = 1;

	sscanf(buf, "%d", &frinode->value);
	sysfs_notify(frinode_kobj, NULL, "frinode_notify");
	if(frinode->value) {
		if (was_on == 0) {
			thread_control = 1;
			thread = kthread_create((int(*)(void*))kt_free_inode, NULL,
						 "kt_free_inode");
			wake_up_process(thread);
		}
	}
	else if (frinode -> value == 0) {
		if (was_on)
			thread_control = 0;
	}
	return len;
}

static struct frinode_attr frinode_value = {
	.attr = __ATTR(frinode_value, 0644, frinode_show, frinode_store),
	.value = 0,
};

static struct frinode_attr frinode_notify = {
	.attr = __ATTR(frinode_notify, 0644, frinode_show, frinode_store),
	.value = 0,
};


/* read_inode put inode in to the queue 
 * so that the thread can periodically wake up
 * and truncate the inode that needs to be deleted
 */
void delay_iput(struct inode *inode)
{
	struct free_inode_t *new = kmem_cache_alloc(allocator, GFP_KERNEL);
	if(new) 
		new -> inode = inode;
	spin_lock(&iq_lock);
	list_add_tail(&new->ls, &block_list);
	spin_unlock(&iq_lock);
}
EXPORT_SYMBOL(delay_iput);

static int kt_free_inode(void)
{
	thread_running = 1;
	while(thread_control) {
		struct free_inode_t *entry;
		/* Do rest of the unlink */
		spin_lock(&iq_lock);
		while (!list_empty(&block_list)) {
			entry = list_first_entry (&block_list,
					struct free_inode_t, ls);
			spin_unlock(&iq_lock);
			if(entry -> inode == NULL) {
				spin_lock(&iq_lock);
				list_del(&entry->ls);
				spin_unlock(&iq_lock);
				kmem_cache_free(allocator, entry);
				spin_lock(&iq_lock);

				continue;
			}

			iput_zero(entry -> inode);
			count_free_blocks++;
			spin_lock(&iq_lock);
			list_del(&entry->ls);
			spin_unlock(&iq_lock);
			kmem_cache_free(allocator, entry);
			spin_lock(&iq_lock);

		}
		spin_unlock(&iq_lock);
		ssleep(1);
	}
	thread_running = 0;
	return 0;
}

static int __init kt_free_inode_init(void) 
{
	int ret = 0;
	thread_control = 0;
	spin_lock_init(&iq_lock);
	INIT_LIST_HEAD(&block_list);
	allocator = kmem_cache_create("delay_iput", sizeof(struct free_inode_t),
			0, 0, NULL);
	if(allocator == NULL) {
		printk(KERN_ERR "%s: kmem cache create failed!!!!\n",__func__);
		return -1;
	}
	frinode_kobj = kobject_create_and_add("free_inode", NULL);
	ret = sysfs_create_group(frinode_kobj, &frinode_group);
	if(ret) {
		printk("%s: sysfs_create_group() failed. ret=%d\n", __func__,
				ret);
	}

	return 0;
}

static void __exit kt_free_inode_cleanup(void)
{
	printk(KERN_ERR "Cleaning up kt_free_inode module...\n");
	if(kthread_stop(thread)){
		printk(KERN_ERR "kt_free_inode: Thread Stopped!\n");
	}
	kmem_cache_shrink(allocator);
}


module_init(kt_free_inode_init);
module_exit(kt_free_inode_cleanup);
