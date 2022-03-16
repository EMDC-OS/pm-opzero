/*
 * Delay free block is mainly for zerooutting the blocks at the background. So
 * that the allocation does not require zeroing. 
 * */

#include "delay_free_block.h"
#include "linux/ktime.h"

static struct task_struct *threads[4];
static struct task_struct *thread;
static struct task_struct *thread2;
static struct task_struct *thread3;
static struct task_struct *thread4;
static struct task_struct *thread_monitoring;
static struct list_head block_list;
static struct kmem_cache* allocator;
static struct kmem_cache* ndctl_alloc;
static spinlock_t fb_list_lock; 
static spinlock_t kt_free_lock;
static unsigned long count_free_blocks;
static unsigned long num_free_blocks;
static unsigned long num_freeing_blocks;
static atomic64_t total_blocks;
static atomic64_t tblocks[4];
static unsigned long read_bytes, write_bytes;
static unsigned long zspeed, zspeed_monitor;
static int thread_control;
static struct ndctl_cmd *pcmd;
static meminfo output[6];
static meminfo res;
static meminfo init_rw[6];
static input_info input;
static struct nd_cmd_vendor_tail *tail;
static dev_t devnum;
// static struct free_block_t *tmp_entry;
static struct block_device *blkdev;
static struct super_block *real_super; 
static int zero_ratio;
static int rc;
static int kt_free_block(void *);
static int manipulate_kthread(unsigned int);
static void monitor_media(void);
static void flush(void);
int was_on=0;
static int cur_num_thread = 1;
static int need_shrink = 0;
static ktime_t start_time, end_time;
static unsigned long zspeed1 = 0, zspeed2 = 0;
static unsigned long total_time;
static char *blkdev_name;

typedef struct zthread {
  struct task_struct *thread;
  struct free_block_t *tmp_entry;
  atomic64_t tblock;
}zthread_t;

static zthread_t bzthreads[4];

static struct kobject *frblk_kobj;

struct frblk_attr{
	struct kobj_attribute attr;
	int value;
};

static struct frblk_attr frblk_value;
static struct frblk_attr frblk_notify;
static struct frblk_attr target_blkdev;
static struct frblk_attr set_speed;

static struct attribute *frblk_attrs[] = {
	&frblk_value.attr.attr,
	&frblk_notify.attr.attr,
	&target_blkdev.attr.attr,
	&set_speed.attr.attr,
	NULL
};

static struct attribute_group frblk_group = {
	.attrs = frblk_attrs,
};


static ssize_t frblk_show(struct kobject *kobj, struct kobj_attribute *attr,
                char *buf)
{
        return scnprintf(buf, PAGE_SIZE,"MediaReads_0: %lu\n"
                                        "MediaWrites_0: %lu\n"
                                        "Zero Ratio: %d\n"
                                        "Zero_speed: %lu\n"
                                        "cur_thread: %d\n"
                                        "Zspeed: %lu\n",
                        read_bytes/(1<<20), write_bytes/(1<<20),
                        zero_ratio, zspeed_monitor, cur_num_thread, zspeed);
}



static ssize_t frblk_store(struct kobject *kobj, struct kobj_attribute *attr,
                const char *buf, size_t len)
{
        struct frblk_attr *frblk = container_of(attr, struct frblk_attr, attr);
        int i;

        sscanf(buf, "%d", &frblk->value);
        sysfs_notify(frblk_kobj, NULL, "frblk_notify");
        if (frblk->value == 1) {
                if (was_on == 0) {
                        for(i = 0; i < 6; i++) {
                                char dev_name[6];
                                snprintf(dev_name, 6, "nmem%d", i);
                                rc = dev_nd_ioctl(dev_name, ND_IOCTL_VENDOR, 
                                        (unsigned long)&pcmd->cmd_buf, DIMM_IOCTL);     
                                if(rc) {
                                        printk(KERN_ERR "%s: Error on nd_ioctl\n", 
                                                __func__);
                                }
                                memcpy(&init_rw[i], tail->out_buf, sizeof(meminfo));
                        }
                        //devnum = ((259 & 0xfff) << 20) | (1 & 0xff);
                        blkdev = lookup_bdev(blkdev_name);
                        if (!blkdev) {
                                printk(KERN_ERR "No block device\n");
                                goto out;
                        }
                        real_super = get_active_super(blkdev);
                        if (!real_super) {
                                printk(KERN_ERR "No super block for blkdev\n");
                                goto out;
                        }

                        thread_monitoring =
                                kthread_create((int(*)(void*))monitor_media, NULL,
                                                "monitor_media");
                        bzthreads[0].thread = kthread_create((int(*)(void*))kt_free_block,
                                        &bzthreads[0], "kt_free_block");
                        /*
                        thread2 = kthread_create((int(*)(void*))kt_free_block2,
                                        NULL, "kt_free_block2");
                        thread3 = kthread_create((int(*)(void*))kt_free_block3,
                                        NULL, "kt_free_block3");
                        thread4 = kthread_create((int(*)(void*))kt_free_block4,
                                        NULL, "kt_free_block4");
                        thread5 = kthread_create((int(*)(void*))kt_free_block,
                                        NULL, "kt_free_block5");
                        */

                        was_on = 1;
                        cur_num_thread = 1;
                        wake_up_process(thread_monitoring);
                        wake_up_process(bzthreads[0].thread);
                        //wake_up_process(thread2);
                        //wake_up_process(thread3);
                        //wake_up_process(thread4);
                        //wake_up_process(thread5);
                }
        }
	else if (frblk->value == 0) {
                if (was_on) {
                        thread_control = 0;
                        was_on = 0;
                        flush();
                        for(i=0; i<4; i++)
                                atomic64_set(&bzthreads[i].tblock, 0);
                        cur_num_thread = 0;
                        deactivate_super(real_super);
                        blkdev = NULL;
                        real_super = NULL;
                }
        }
        else if (frblk->value == 3) {
                //flush();
        }
out:
        return len;
}


static struct frblk_attr frblk_value = {
	.attr = __ATTR(frblk_value, 0644, frblk_show, frblk_store),
	.value = 0,
};


/*
static struct frblk_attr frblk_value = {
	.attr = __ATTR(frblk_value, 0644, frblk_show, frblk_store),static ssize_t blkdevname_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	sscanf(buf, "%s", blkdev_name);
        return len;
}
	.value = 0,
};
*/

static struct frblk_attr frblk_notify = {
	.attr = __ATTR(frblk_notify, 0644, frblk_show, frblk_store),
	.value = 0,
};

static ssize_t blkdevname_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", blkdev_name);
}

static ssize_t blkdevname_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	sscanf(buf, "%s", blkdev_name);
        return len;
}

static struct frblk_attr target_blkdev = {
	.attr = __ATTR(target_blkdev, 0644, blkdevname_show, blkdevname_store),
	.value = 0,
};

static ssize_t set_speed_show(struct kobject *kobj, struct kobj_attribute *attr,
                char *buf)
{
        return scnprintf(buf, PAGE_SIZE, "%lu\n", zspeed);
}

static ssize_t set_speed_store(struct kobject *kobj, struct kobj_attribute *attr,
                const char *buf, size_t len)
{
        sscanf(buf, "%lu", &zspeed);
        return len;
}

static struct frblk_attr set_speed = {
        .attr = __ATTR(set_speed, 0644, set_speed_show, set_speed_store),
        .value = 0,
};


/* Get total blocks in the free_block list
 * */
long xfs_get_num_pz_blocks(void)
{
	return atomic64_read(&total_blocks);
}
EXPORT_SYMBOL(xfs_get_num_pz_blocks);

void xfs_delay_free_block(struct xfs_mount *mp, xfs_fsblock_t start_block, xfs_extlen_t len, uint64_t oi_owner, 
		xfs_fileoff_t oi_offset, unsigned int oi_flags, bool skip_discard)
{
	/* 
	 * Here we need to put the information into the list.
	 * Need to use kmem_cache_alloc
	 * */
	struct free_block_t *new = kmem_cache_alloc(allocator, GFP_KERNEL);
	if(new) {
		new->mp = mp;
		new->start_block = start_block;
		new->len = len;
		new->oi_owner = oi_owner;
		new->oi_offset = oi_offset;
		new->oi_flags = oi_flags;
		new->skip_discard = skip_discard;
	}

	spin_lock(&fb_list_lock);
	list_add_tail(&new->ls, &block_list);
	spin_unlock(&fb_list_lock);
	/* Need the count for number of total blocks in the list
	 * Should be handled with locks
	 * */
	atomic64_add(len, &total_blocks);
}
EXPORT_SYMBOL(xfs_delay_free_block);

static inline loff_t 
xfs_iomap_dax_zero_range(loff_t pos, loff_t count, struct iomap *iomap)
{
	loff_t written = 0;
	int status;

	do {
		unsigned offset, bytes;

		offset = offset_in_page(pos);
		bytes = min_t(loff_t, PAGE_SIZE - offset, count);

		status = dax_iomap_zero(pos, offset, bytes, iomap);
		if (status < 0)
			return status;

		pos += bytes;
		count -= bytes;
		written += bytes;
	} while (count > 0);

	return written;
}

static int free_blocks(struct free_block_t *entry)
{
	struct xfs_owner_info oinfo;
	struct xfs_mount	*mp = entry->mp;
	struct xfs_trans	*tp;
	struct super_block *sp = mp->m_super;
	struct iomap dax_iomap;
	int error;
	loff_t written = 0;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0, &tp);
	if (error) {
		ASSERT(XFS_FORCED_SHUTDOWN(mp));
		return error;
	}

	dax_iomap.addr = BBTOB(XFS_FSB_TO_DADDR(mp, entry->start_block));
	dax_iomap.offset = 0;
	dax_iomap.length = XFS_FSB_TO_B(mp, entry->len);
	dax_iomap.bdev = blkdev;
	dax_iomap.dax_dev = fs_dax_get_by_bdev(blkdev);

	//written = xfs_iomap_dax_zero_range(0, sp->s_blocksize * entry->len, 
	written = xfs_iomap_dax_zero_range(0, dax_iomap.length, 
			&dax_iomap);

	if (!written)
		goto commit_out;

	if (entry->oi_owner == 0 && 
	    entry->oi_offset == 0 && entry->oi_flags == 0)
		error = __xfs_free_extent(tp, entry->start_block, entry->len,
				NULL, XFS_AG_RESV_NONE, entry->skip_discard);
	else {
		oinfo.oi_owner = entry->oi_owner;
		oinfo.oi_offset = entry->oi_offset;
		oinfo.oi_flags = entry->oi_flags;

		error = __xfs_free_extent(tp, entry->start_block, entry->len,
				&oinfo, XFS_AG_RESV_NONE, entry->skip_discard);
	}

commit_out:
	error = xfs_trans_commit(tp);

	return error;
}

int xfs_free_num_blocks(xfs_extlen_t len, struct free_block_t *tmp_entry) 
{
	struct free_block_t *entry;
	int err;

	while(len){
		err = 0;
		spin_lock(&fb_list_lock);
                if (list_empty(&block_list)) {
                  spin_unlock(&fb_list_lock);
                  break;
                } 
		entry = list_first_entry(&block_list,
				struct free_block_t, ls);
		//Check if this entry has more blocks than needed
		if(len < entry->len) {
			//Free only needed amount and
			//update the entry info (pblk, count)
			tmp_entry->mp = entry->mp;
			tmp_entry->start_block = entry->start_block;
			tmp_entry->len = len;
			tmp_entry->oi_owner = entry->oi_owner;
			tmp_entry->oi_offset = entry->oi_offset;
			tmp_entry->oi_flags = entry->oi_flags;
			tmp_entry->skip_discard = entry->skip_discard;
			entry->start_block += len;
			entry->len -= len;
			spin_unlock(&fb_list_lock);

			err = free_blocks(tmp_entry);
			if(err)
				printk(KERN_ERR "%s: ERROR on freeing extent\n",
					__func__);
			atomic64_sub(len, &total_blocks);

			break;

		} else { // Free this entry and subtract the amount from count
			tmp_entry->mp = entry->mp;
			tmp_entry->start_block = entry->start_block;
			tmp_entry->len = entry->len;
			tmp_entry->oi_owner = entry->oi_owner;
			tmp_entry->oi_offset = entry->oi_offset;
			tmp_entry->oi_flags = entry->oi_flags;
			tmp_entry->skip_discard = entry->skip_discard;
			list_del(&entry->ls);
			
			kmem_cache_free(allocator, entry);
			spin_unlock(&fb_list_lock);

			len -= tmp_entry->len;
			err = free_blocks(tmp_entry);
			if(err)
				printk(KERN_ERR "%s: ERROR on freeing extent\n",
					__func__);

			atomic64_sub(tmp_entry->len, &total_blocks);
		}

	}
	return 1;
}
EXPORT_SYMBOL(xfs_free_num_blocks);


static int kt_free_block(void *data)
{
        //ktime_t start_time, end_time;
        //struct timeval startTime, endTime;
        unsigned long th_zero_time;
        atomic64_t *cblk = &((zthread_t*)data)->tblock;
        int worked = 0;
        unsigned long elapsed_time = 0;
        // struct free_block_t *tmp_entry = ((zthread_t*)data)->tmp_entry;
	struct free_block_t *tmp_entry = kmem_cache_alloc(allocator, GFP_KERNEL);
	
        while(was_on) {
                while (atomic64_read(cblk) > 0) {
                    long cnt = min_t(u64, (long)atomic64_read(cblk), 1000);
                    if((long)atomic64_read(&total_blocks) >= cnt) {
                        start_time = ktime_get();
                        //gettimeofday(&startTime, NULL);
                        atomic64_sub(cnt, cblk);
                        xfs_free_num_blocks(cnt, tmp_entry);
                        num_freeing_blocks += cnt;
                        //gettimeofday(&endTime, NULL);
                        //elapsed_time += ( endTime.tv_sec - startTime.tv_sec );
                        end_time = ktime_get();
                        elapsed_time += ktime_sub(end_time, start_time);
                        //th_zero_time = ktime_to_ns(ktime_sub(end_time, start_time));
                    }
                    cond_resched();
                    worked = 1;
		}
                //sleep_time = 1000000000/((zspeed)/40);
                //if (sleep_time > th_zero_time) {
                if (worked) {
                  th_zero_time = ktime_to_ns(elapsed_time);
                  //printk(KERN_ERR "elapsedTime : %lu ns\n",  th_zero_time);
                  if (th_zero_time < 1000000000) {
                    msleep((1000000000-th_zero_time)/1000000);
                    if (th_zero_time < 500000000)
                      need_shrink = 1;
                    else
                      need_shrink = 0;
                  }
                }
                worked = 0;
                if (kthread_should_stop()) {
                  //printk(KERN_ERR "Stop Kthread\n");
		  kmem_cache_free(allocator, tmp_entry);
                  return 0;
                }
                cond_resched();
        }
	kmem_cache_free(allocator, tmp_entry);
        return 0;
}


static void flush(void)
{
	struct free_block_t *entry;
	int err;
	spin_lock(&fb_list_lock);
	while (!list_empty(&block_list)) {
		err = 0;
		entry = list_first_entry(&block_list,
				struct free_block_t, ls);
		list_del(&entry -> ls);
		spin_unlock(&fb_list_lock);

		num_freeing_blocks += entry->len;
		err = free_blocks(entry);
		num_free_blocks += entry->len;
		
		atomic64_sub(entry->len, &total_blocks);
		kmem_cache_free(allocator, entry);
		count_free_blocks++;

		/* This lock is for loop condition check */
		spin_lock(&fb_list_lock);
	}
	spin_unlock(&fb_list_lock);

        atomic64_set(&total_blocks, 0);
}

static void monitor_media(void)
{
	// Get superblock from dev num
	struct xfs_mount *mp;
	uint64_t zblocks;
	int i, incomplete = 0;
        int prev_free = 0;
	mp = XFS_M(real_super);
	zblocks = atomic64_read(&total_blocks);
	while(was_on) {
	//	int idle = 0;
		char dev_name[6];
		u64 bfree, pz_blocks;
		unsigned long read_write, zio, zfree;
//		printk(KERN_ERR "%s: Keep monitoring...\n", __func__);
		unsigned int margin = 80 * cur_num_thread;
                int need_thread = 0, append = 0;
                incomplete = 0;

		res.MediaReads.Uint64 = 0;
		res.MediaReads.Uint64_1 = 0;
		res.MediaWrites.Uint64 = 0;
		res.MediaWrites.Uint64_1 = 0;

		for(i = 0; i < 6; i++) {
			snprintf(dev_name, 6, "nmem%d", i);
			rc = dev_nd_ioctl(dev_name, ND_IOCTL_VENDOR, 
				(unsigned long)&pcmd->cmd_buf, DIMM_IOCTL);	
			if(rc) 
				printk(KERN_ERR "%s: Error on nd_ioctl(%d)\n", __func__, rc);
			memcpy(&output[i], tail->out_buf, sizeof(meminfo));
		//	printk(KERN_ERR "%s: read: %llu\n"
		//			"write: %llu\n",
		//			__func__,
		//			output[i].MediaReads.Uint64,
		//			output[i].MediaWrites.Uint64);
			res.MediaReads.Uint64 += output[i].MediaReads.Uint64 -
						 init_rw[i].MediaReads.Uint64;
			res.MediaReads.Uint64_1 += output[i].MediaReads.Uint64_1
						   - init_rw[i].MediaReads.Uint64_1;
			res.MediaWrites.Uint64 += output[i].MediaWrites.Uint64 -
						  init_rw[i].MediaWrites.Uint64;
			res.MediaWrites.Uint64_1 += output[i].MediaWrites.Uint64_1
						    - init_rw[i].MediaWrites.Uint64_1;
			init_rw[i].MediaReads.Uint64 = output[i].MediaReads.Uint64;
			init_rw[i].MediaReads.Uint64_1 = output[i].MediaReads.Uint64_1;
			init_rw[i].MediaWrites.Uint64 = output[i].MediaWrites.Uint64;
			init_rw[i].MediaWrites.Uint64_1 = output[i].MediaWrites.Uint64_1;

		}
		/* Calculate the current speed of read and write
		 * Also needs to store before, current and take good care of
		 * number of freed amount of zeroed blocks with current I/O
		 * speed
		 * */
		read_bytes = res.MediaReads.Uint64 * 64 < num_freeing_blocks * 4096 ?
					0 : res.MediaReads.Uint64*64
						- num_freeing_blocks*4096;
		write_bytes = res.MediaWrites.Uint64 * 64 < num_freeing_blocks * 4096 ?
				0 : res.MediaWrites.Uint64*64
					- num_freeing_blocks * 4096;

		zspeed_monitor = (num_freeing_blocks*4096)/(1<<20);
                //printk(KERN_ERR "before zspeed %lu\n", zspeed);
                // long long int total_time = ktime_to_ns(elapsed_time) + ktime_to_ns(elapsed_time);
                /*
                if(total_time > 0){
                        printk(KERN_ERR "zspeed and elapsed_time: %lu MB/s %lu ns\n", zspeed_monitor, total_time);
                }
                else{
                        printk(KERN_ERR "total time is 0 or negative\n");
                }
                */

		bfree =	percpu_counter_sum(&mp->m_fdblocks);
                if (prev_free > bfree + num_freeing_blocks)
                        append = 1;
                prev_free = bfree;
                num_freeing_blocks = 0;

                read_write = (10*read_bytes/25+write_bytes)/(1<<20);
                if (read_write < 100){
                        zio = 8000;
                } else if (read_write > 4500 && append) {
                        zio = 500;
                } else if (read_write <= 4500 && append) {
                        zio = 1500;
                } else {
                        zio = 100;
                }

                if (append) {
                        pz_blocks = (u64) atomic64_read(&total_blocks);
                        zero_ratio = 100 * pz_blocks / ( bfree + pz_blocks );

                        if(zero_ratio <= 10)
                                zfree = 150;
                        else if(zero_ratio <= 20)
                                zfree = 304;
                        else if(zero_ratio <= 30)
                                zfree = 464;
                        else if(zero_ratio <= 40)
                                zfree = 635;
                        else if(zero_ratio <= 50)
                                zfree = 823;
                        else if(zero_ratio <= 60)
                                zfree = 1039;
                        else if(zero_ratio <= 70)
                                zfree = 1300;
                        else if(zero_ratio <= 80)
                                zfree = 1647;
                        else if(zero_ratio <= 90)
                                zfree = 2208;
                        else
                                zfree = 3969;
                } else  {
                        zero_ratio = -1;
                        zfree = 0;
                }

		zspeed = max(zio, zfree);
                //zio = min_t(u64, 8000 - read_write, 4000);

                for (i=0; i < cur_num_thread; i++) {
                  if (atomic64_read(&bzthreads[i].tblock) > 0)
                    incomplete = 1;
                  if (incomplete)
                    break;
                }

                if (zspeed_monitor && incomplete) {
                  need_thread = min_t(u64, (zspeed / (zspeed_monitor / cur_num_thread)) + 1, 4);
                }
                else if (need_shrink || zspeed_monitor == 0) {
                  need_thread = max_t(u64, cur_num_thread - 1, 1);
                }
                else
                  need_thread = cur_num_thread;

                for (i=0; i < need_thread; i++)
                  atomic64_set(&bzthreads[i].tblock, (zspeed*1024/4)/need_thread);
                for (i=need_thread; i < 4; i++)
                  atomic64_set(&bzthreads[i].tblock, 0);

                if (need_thread >= 1) {
                  // if same, just passed
                  cur_num_thread = manipulate_kthread(need_thread);
                }


                /*
                if(zspeed > 2000){
                        zspeed1 = zspeed/2;
                        zspeed2 = zspeed - zspeed1;
                }
                else{
                        zspeed1 = zspeed;
                        zspeed2 = 0;
                }
                */


                /*
                if (read_write >= 8000)
                        read_write = 8000;
                
                
                */

                // zspeed = 8000-read_write;

                msleep(1000);
        }
}

int manipulate_kthread(unsigned int need_thread) {
  int i;

  if (need_thread < cur_num_thread) {
    //stop kthread
    for (i = need_thread; i < cur_num_thread; i++) {
      kthread_stop(bzthreads[i].thread);
    }
  }
  else if (need_thread > cur_num_thread){
    //create kthread
    for (i = cur_num_thread; i < need_thread; i++) {
      bzthreads[i].thread = kthread_create((int(*)(void*))kt_free_block,
          &bzthreads[i], "kt_free_block");
      printk(KERN_ERR "Create Kthread %d\n", i+1);
      wake_up_process(bzthreads[i].thread);
    }
  }

  return need_thread;
}

static int __init kt_free_block_init(void) 
{
	int i, ret = 0;
	size_t size;
	rc = 0;
	count_free_blocks = 0;
	num_free_blocks = 0;
	num_freeing_blocks = 0;
	atomic64_set(&total_blocks, 0);
	thread_control = 0;
	input.MemoryPage = 0x0;
	res.MediaReads.Uint64 = 0;
	res.MediaReads.Uint64_1 = 0;
	res.MediaWrites.Uint64 = 0;
	res.MediaWrites.Uint64_1 = 0;
	read_bytes = 0;
	write_bytes = 0;
	zspeed = 1;
	spin_lock_init(&fb_list_lock);
	INIT_LIST_HEAD(&block_list);
	allocator = kmem_cache_create("delay_free_block", sizeof(struct
				free_block_t), 0, 0, NULL);
	if(allocator == NULL) {
		printk(KERN_ERR "Kmem_cache create failed!!!!\n");
		return -1;
	}

	blkdev_name = kmalloc(30, GFP_KERNEL);
        if (!blkdev_name) {
		printk(KERN_ERR "kmalloc for dev name failed!!!!\n");
		return -1;
        }
        strncpy(blkdev_name, "/dev/pmem0", 10);

	// tmp_entry = kmem_cache_alloc(allocator, GFP_KERNEL);

	frblk_kobj = kobject_create_and_add("free_block_xfs", NULL);
	ret = sysfs_create_group(frblk_kobj, &frblk_group);
	if(ret) {
		printk("%s: sysfs_create_group() failed. ret=%d\n", __func__,
				ret);
	}

	/* Initialize nd_ioctl commands and buffer
	 * */
	size = sizeof(*pcmd) + sizeof(struct nd_cmd_vendor_hdr) 
		+ sizeof(struct nd_cmd_vendor_tail) + 128 + 128;
	ndctl_alloc = kmem_cache_create("delay_free_block", size, 0, 0, NULL);
	pcmd = kmem_cache_alloc(ndctl_alloc, GFP_KERNEL);
	pcmd->type = ND_CMD_VENDOR;
	pcmd->size = size;
	pcmd->status = 1;
	pcmd->vendor->opcode = (uint32_t) (0x03 << 8 | 0x08);
	pcmd->vendor->in_length = 128;
	memcpy(pcmd->vendor->in_buf, &input, sizeof(input_info));
	tail = (struct nd_cmd_vendor_tail *) 
		(pcmd->cmd_buf + sizeof(struct nd_cmd_vendor_hdr)
		 + pcmd->vendor->in_length);
	tail->out_length = (u32) 128;
	
        for (i=0; i<4; i++) {
          atomic64_set(&bzthreads[i].tblock, 0);
          bzthreads[i].tmp_entry = kmem_cache_alloc(allocator, GFP_KERNEL);
        }
	// threads[0] = thread;
 //        threads[1] = thread2;
 //        threads[2] = thread3;
 //        threads[3] = thread4;

	return 1;
}

static void __exit kt_free_block_cleanup(void)
{
        int i;
	printk(KERN_INFO "Cleaning up kt_free_block module...\n");
	kmem_cache_free(ndctl_alloc, pcmd);
        for (i=0; i<4; i++)
          kmem_cache_free(allocator, bzthreads[i].tmp_entry);
	kfree(blkdev_name);
	kmem_cache_shrink(allocator);
	kmem_cache_shrink(ndctl_alloc);
}


module_init(kt_free_block_init);
module_exit(kt_free_block_cleanup);

