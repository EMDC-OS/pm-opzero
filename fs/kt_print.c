#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <asm/spinlock.h>

static struct task_struct *thread;
static struct inode ** inode_queue;
static int queue_index;
static spinlock_t iq_lock;

#ifndef MAX_INODE_QUEUE 
#define MAX_INODE_QUEUE 8000
#endif

/* read_inode put inode in to the queue 
 * so that the thread can periodically wake up
 * and truncate the inode that needs to be deleted
 */
int delay_iput(struct inode *inode)
{
	if(inode){
		if((queue_index + 1) > MAX_INODE_QUEUE){
			printk(KERN_ERR "End of cicular inode queue!\n");
			queue_index = 0;
		}
		
		while (inode_queue[queue_index] != NULL) {
			/* unlikely */
		}
		
		spin_lock(&iq_lock);
		inode_queue[queue_index++]=inode;		
		spin_unlock(&iq_lock);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(delay_iput);

static int kt_print(void)
{
	int i = 0; struct inode *inode;
	while(!kthread_should_stop()){
		/* Do rest of the unlink */
		while (1) {
			spin_lock(&iq_lock);
			if(inode_queue[i] == NULL) {
				spin_unlock(&iq_lock);
				break;
			}
			inode = inode_queue[i];
			inode_queue[i] = NULL;
			spin_unlock(&iq_lock);

			iput(inode);

			i++;
			if( i >= MAX_INODE_QUEUE) i = 0;
		}
		ssleep(1);
	}

	return 0;
}

static int __init kt_print_init(void) 
{

	queue_index = 0;
	inode_queue = kmalloc(sizeof(struct inode*)*8000, GFP_KERNEL);
	thread = kthread_create((int(*)(void*))kt_print, NULL, "kt_print");
	if(thread){
		wake_up_process(thread);
	}

	return 0;
}

static void __exit kt_print_cleanup(void)
{
	printk(KERN_ERR "Cleaning up kt_print module...\n");
	if(kthread_stop(thread)){
		printk(KERN_ERR "kt_print: Thread Stopped!\n");
	}
	kfree(inode_queue);
}


module_init(kt_print_init);
module_exit(kt_print_cleanup);
