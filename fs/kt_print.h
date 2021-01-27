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

int delay_iput(struct inode *inode);
