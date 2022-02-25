/*
 * Delay free block is mainly for zerooutting the blocks at the background. So
 * that the allocation does not require zeroing. 
 * */

#include "delay_free_block_test.h"
#include "linux/fs.h"
//#include "linux/kernel.h"
//#include "linux/hrtimer.h"
#include "linux/ktime.h"

static struct task_struct *threads[4];
static struct task_struct *thread;
static struct task_struct *thread2;
static struct task_struct *thread3;
static struct task_struct *thread4;
//static struct task_struct *thread5;
static struct task_struct *thread_monitoring;
static struct list_head block_list;
static struct kmem_cache* allocator;
static struct kmem_cache* ndctl_alloc;
static struct kmem_cache* ext4_free_data_cachep;
static spinlock_t fb_list_lock; 
static spinlock_t kt_free_lock; 
static unsigned long count_free_blocks;
static unsigned long num_free_blocks;
static unsigned long num_freeing_blocks;
static atomic64_t total_blocks;
static unsigned long read_bytes, write_bytes;
static unsigned long zspeed, zspeed_monitor;
static int thread_control;
static struct ndctl_cmd *pcmd;
static meminfo output[6];
static meminfo res;
static meminfo init_rw[6];
static input_info input;
static struct nd_cmd_vendor_tail *tail;
static char *blkdev_name;
struct block_device *blkdev;
struct super_block *real_super;
static struct free_block_t *tmp_entry;
static int zero_ratio;
static int rc;
static int kt_free_block(void);
static int manipulate_kthread(unsigned int);
static void monitor_media(void);
static void flush(void);
static int was_on = 0;
static int cur_num_thread = 1;
static int need_shrink = 0;
static ktime_t elapsed_time = 0, start_time, end_time;
static unsigned long zspeed1 = 0, zspeed2 = 0;
static unsigned long total_time;

#define TEST

void ext4_delay_free_block(struct inode * inode, ext4_fsblk_t block, 
		unsigned long count, int flag);

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
					"Zero_speed: %lu\n",
			read_bytes/(1<<20), write_bytes/(1<<20),
			zero_ratio, zspeed_monitor);
}

static ssize_t frblk_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	struct frblk_attr *frblk = container_of(attr, struct frblk_attr, attr);

	sscanf(buf, "%d", &frblk->value);
	sysfs_notify(frblk_kobj, NULL, "frblk_notify");
	if (frblk->value == 1) {
		if (was_on == 0) {
			int i;
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
			thread = kthread_create((int(*)(void*))kt_free_block,
					NULL, "kt_free_block");
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
			wake_up_process(thread);
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
long get_num_pz_blocks(void)
{
	return atomic64_read(&total_blocks);
}
EXPORT_SYMBOL(get_num_pz_blocks);

/* ext4_delay_free_block
 * Store freeing blocks in to the list
 */
void ext4_delay_free_block(struct inode * inode, ext4_fsblk_t block, 
		unsigned long count, int flag)
{
	
	spin_lock(&fb_list_lock);
	struct free_block_t *new = kmem_cache_alloc(allocator, GFP_KERNEL);
	if(new) {
		new -> inode = inode;
		new -> block = block;
		new -> flag = flag;
		new -> count = count;
	}

	list_add_tail(&new -> ls, &block_list);
	spin_unlock(&fb_list_lock);

	atomic64_add(count, &total_blocks);
}
EXPORT_SYMBOL(ext4_delay_free_block);

static inline loff_t 
ext4_iomap_dax_zero_range(loff_t pos, loff_t count, struct iomap *iomap)
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

/* We need to free blocks in the list one by one
 * Need to clear the block bitmap, zeroout the blocks we
 * have, and change the associated group descriptor
 */
static int free_blocks(struct free_block_t *entry)
{
	struct inode *inode = entry -> inode;
	ext4_fsblk_t block = entry -> block;
	unsigned long count = entry -> count;
	int flags = entry -> flag;
	unsigned int overflow;
	struct super_block *sb = real_super; 
	struct ext4_group_desc *gdp;
	ext4_grpblk_t bit;
	ext4_group_t block_group;
	struct buffer_head *gd_bh;
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_buddy e4b;
	unsigned int count_clusters;
	int err = 0;
	int ret;
	struct iomap dax_iomap;
	loff_t written = 0;
	handle_t *handle = NULL;

do_more:
	overflow = 0;
	ext4_get_group_no_and_offset(sb, block, &block_group,
			&bit);
	/* We need to free blocks in the list one by one
	 * Need to clear the block bitmap, zeroout the blocks we
	 * have, and change the associated group descriptor
	 * */
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT( ext4_get_group_info(sb,
						block_group))))
		return 1;
	if (EXT4_C2B(sbi, bit) + count > EXT4_BLOCKS_PER_GROUP(sb)) {
		overflow = EXT4_C2B(sbi, bit) + count -
			EXT4_BLOCKS_PER_GROUP(sb);
		count -= overflow;
	}
	count_clusters = EXT4_NUM_B2C(sbi, count);
	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (IS_ERR(bitmap_bh)) {
		err = PTR_ERR(bitmap_bh);
		bitmap_bh = NULL;
		goto error_return;
	}
	gdp = ext4_get_group_desc(sb, block_group, &gd_bh);
	if (!gdp) {
		err = -EIO;
		goto error_return;
	}

//	if (in_range(ext4_block_bitmap(sb, gdp), block, count) ||
//			in_range(ext4_inode_bitmap(sb, gdp), block, count) ||
//			in_range(block, ext4_inode_table(sb, gdp),
//				sbi->s_itb_per_group) ||
//			in_range(block + count - 1, ext4_inode_table(sb, gdp),
//				sbi->s_itb_per_group)) {
//
//		ext4_error(sb, "Freeing blocks in system zone - "
//				"Block = %llu, count = %lu", block, count);
//		/* err = 0. ext4_std_error should be a no op */
//		goto error_return;
//	}

	/* Modified for zeroout data blocks while trucate for dax
	 * */
	/* use iomap_zero_range need to find from and length */
	dax_iomap.addr = block << inode->i_blkbits;
	dax_iomap.offset = 0;
	dax_iomap.length = real_super->s_blocksize * count; 
	dax_iomap.bdev = blkdev;
	dax_iomap.dax_dev = fs_dax_get_by_bdev(blkdev);
	written = ext4_iomap_dax_zero_range(0, dax_iomap.length, 
				&dax_iomap);

	/* New handle for journaling
	 * */
//	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
//		credits = ext4_writepage_trans_blocks(inode);
//	else
//		credits = ext4_blocks_for_truncate(inode);
//	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);

	/* __GFP_NOFAIL: retry infinitely, ignore TIF_MEMDIE and memcg limit. */
	err = ext4_mb_load_buddy_gfp(sb, block_group, &e4b,
			GFP_NOFS|__GFP_NOFAIL);
	if (err)
		goto error_return;


	/*
	 * We need to make sure we don't reuse the freed block until after the
	 * transaction is committed. We make an exception if the inode is to be
	 * written in writeback mode since writeback mode has weak data
	 * consistency guarantees.
	 */
	if (0 && ext4_handle_valid(handle) &&
			((flags & EXT4_FREE_BLOCKS_METADATA) ||
			 !ext4_should_writeback_data(inode))) {
		struct ext4_free_data *new_entry;
		/*
		 * We use __GFP_NOFAIL because ext4_free_blocks() is not allowed
		 * to fail.
		 */
		new_entry = kmem_cache_alloc(ext4_free_data_cachep,
				GFP_NOFS|__GFP_NOFAIL);
		new_entry->efd_start_cluster = bit;
		new_entry->efd_group = block_group;
		new_entry->efd_count = count_clusters;
		new_entry->efd_tid = handle->h_transaction->t_tid;

		ext4_lock_group(sb, block_group);
		mb_clear_bits(bitmap_bh->b_data, bit, count_clusters);
		ext4_mb_free_metadata(handle, &e4b, new_entry);
	} else {
		/* need to update group_info->bb_free and bitmap
		 * with group lock held. generate_buddy look at
		 * them with group lock_held
		 */
		if (test_opt(sb, DISCARD)) {
			err = ext4_issue_discard(sb, block_group, bit, count,
					NULL);
			if (err && err != -EOPNOTSUPP)
				ext4_msg(sb, KERN_WARNING, "discard request in"
						" group:%d block:%d count:%lu failed"
						" with %d", block_group, bit, count,
						err);
		} else
			EXT4_MB_GRP_CLEAR_TRIMMED(e4b.bd_info);

		ext4_lock_group(sb, block_group);
		mb_clear_bits(bitmap_bh->b_data, bit, count_clusters);
		mb_free_blocks(inode, &e4b, bit, count_clusters);
	}

	ret = ext4_free_group_clusters(sb, gdp) + count_clusters;
	ext4_free_group_clusters_set(sb, gdp, ret);
	ext4_block_bitmap_csum_set(sb, block_group, gdp, bitmap_bh);
	ext4_group_desc_csum_set(sb, block_group, gdp);
	ext4_unlock_group(sb, block_group);

	if (sbi->s_log_groups_per_flex) {
		ext4_group_t flex_group = ext4_flex_group(sbi, block_group);
		atomic64_add(count_clusters,
				&sbi_array_rcu_deref(sbi, s_flex_groups,
					flex_group)->free_clusters);
	}

	/*
	 * on a bigalloc file system, defer the s_freeclusters_counter
	 * update to the caller (ext4_remove_space and friends) so they
	 * can determine if a cluster freed here should be rereserved
	 */
	if (!(flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)) {
		if (!(flags & EXT4_FREE_BLOCKS_NO_QUOT_UPDATE))
			dquot_free_block_nodirty(inode, EXT4_C2B(sbi, count_clusters));
		percpu_counter_add(&sbi->s_freeclusters_counter,
				count_clusters);
	}

	ext4_mb_unload_buddy(&e4b);
//	ext4_journal_stop(handle);

	if (overflow && !err) {
		block += count;
		count = overflow;
		put_bh(bitmap_bh);
		goto do_more;
	}
error_return:
	brelse(bitmap_bh);
	ext4_std_error(sb, err);

	/* TODO: For unlink inode, you should set inode new bit after all the
	 * blocks have been freed and zeroed, to reuse that inode
	 * Need to make a tree to store the count of contiguous blocks of inode
	 * which are getting freed later and when the count is zero, inode can
	 * be freed finally
	 * */

	return 0;
}

/* TODO: Free and zeroout those blocks for nclusters
 * Number of pre-zero blocks must be checked before
 * Called by ext4_has_free_clusters()
 * */
int ext4_free_num_blocks(long count) 
{
	struct free_block_t *entry;
	int err;

	while(count){
		err = 0;
//		printk(KERN_ERR "%s: in lock\n", __func__);
//
		spin_lock(&fb_list_lock);
		entry = list_first_entry(&block_list,
				struct free_block_t, ls);
		//Check if this entry has more blocks than needed
		if(count < entry->count) {
			//Free only needed amount and
			//update the entry info (pblk, count)
			tmp_entry->inode = entry -> inode;
			tmp_entry->block = entry -> block;
			tmp_entry->count = count;

			entry -> block += count;
			entry -> count -= count;
			spin_unlock(&fb_list_lock);

			err = free_blocks(tmp_entry);
			atomic64_sub(count, &total_blocks);

			break;

		} else { // Free this entry and subtract the amount from count
			tmp_entry->inode = entry -> inode;
			tmp_entry->block = entry -> block;
			tmp_entry->count = entry -> count;
			list_del(&entry -> ls);
			kmem_cache_free(allocator, entry);
			spin_unlock(&fb_list_lock);

			count -= tmp_entry->count;
			err = free_blocks(tmp_entry);
			
			atomic64_sub(tmp_entry->count, &total_blocks);
		}
	}
	return 1;
}
EXPORT_SYMBOL(ext4_free_num_blocks);

static int kt_free_block(void)
{
	//ktime_t start_time, end_time;
	//struct timeval startTime, endTime;
        unsigned long sleep_time;
        unsigned long th1_zero_time;

	/*
	while(was_on) {
		if((long)atomic64_read(&total_blocks) >= 10000) {
                        //spin_lock(&kt_free_lock);
			ext4_free_num_blocks(10000);
                        //spin_unlock(&kt_free_lock);
			num_freeing_blocks += 10000;
			if(zspeed > 40)
				msleep(1000/(zspeed/40)-1);
		} else {
			msleep(10000);
		}
	}
	*/
	// without speed control
	while(was_on) {
		//long cnt = (long)atomic64_read(&total_blocks);
		if((long)atomic64_read(&total_blocks) >= 10000) {
			start_time = ktime_get();
			//gettimeofday(&startTime, NULL);
			ext4_free_num_blocks(10000);
			num_freeing_blocks += 10000;
			//gettimeofday(&endTime, NULL);
    			//elapsed_time += ( endTime.tv_sec - startTime.tv_sec );
			end_time = ktime_get();
			elapsed_time += ktime_sub(end_time, start_time);
                        th1_zero_time = ktime_to_ns(ktime_sub(end_time, start_time));
                        
			//printk("elapsedTime : %lld ns\n",  ktime_to_ns(elapsed_time));
			if(zspeed > 40) {
                                sleep_time = 1000000000/((zspeed)/40);
                                //printk(KERN_ERR "sleepTime: %lu, elapsedTime : %lu ns\n", sleep_time, th1_zero_time);
                                if (sleep_time > th1_zero_time) {
                                  msleep((sleep_time - th1_zero_time)/1000000);
                                  need_shrink = 1;
                                }
                                else {
                                  need_shrink = 0;
                                }
                        }
		}
                if (kthread_should_stop()) {
                  printk(KERN_ERR "Stop Kthread\n");
                  return 0;
                }
                cond_resched();
	}
	return 0;
}

static int kt_free_block2(void)
{

	ktime_t start_time, end_time;

	while(was_on) {
		if((long)atomic64_read(&total_blocks) >= 10000 && zspeed2 != 0) {
                        //spin_lock(&kt_free_lock);
			start_time = ktime_get();
			ext4_free_num_blocks(10000);
                        //spin_unlock(&kt_free_lock);
			num_freeing_blocks += 10000;
			end_time = ktime_get();
			//elapsed_time2 += ktime_sub(end_time, start_time);
			msleep(1);
		} else {
			msleep(1000);
		}
	}
}


static int kt_free_block3(void)
{
	ktime_t start_time, end_time;
	while(was_on) {
		//long cnt = (long)atomic64_read(&total_blocks);
		if((long)atomic64_read(&total_blocks) >= 10000) {
			start_time = ktime_get();
			ext4_free_num_blocks(10000);
			num_freeing_blocks += 10000;
			end_time = ktime_get();
			elapsed_time += ktime_sub(end_time, start_time);
			//printk("elapsedTime : %lld ns\n",  ktime_to_ns(elapsed_time));
		}
		msleep(1);
	}
	return 0;
}

static int kt_free_block4(void)
{
	ktime_t start_time, end_time;
	while(was_on) {
		//long cnt = (long)atomic64_read(&total_blocks);
		if((long)atomic64_read(&total_blocks) >= 10000) {
			start_time = ktime_get();
			ext4_free_num_blocks(10000);
			num_freeing_blocks += 10000;
			end_time = ktime_get();
			elapsed_time += ktime_sub(end_time, start_time);
			//printk("elapsedTime : %lld ns\n",  ktime_to_ns(elapsed_time));
		}
		msleep(1);
	}
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
		if (entry -> inode == NULL) {
			printk(KERN_ERR "NULL inode in flush\n");
			list_del(&entry -> ls);
			spin_unlock(&fb_list_lock);
			atomic64_sub(entry-> count, &total_blocks);
			spin_lock(&fb_list_lock);
			kmem_cache_free(allocator, entry);
			continue;
		}
		list_del(&entry -> ls);
		spin_unlock(&fb_list_lock);

		num_freeing_blocks += entry->count;
		err = free_blocks(entry);
		num_free_blocks += entry->count;
		
		atomic64_sub(entry->count, &total_blocks);
		count_free_blocks++;

		spin_lock(&fb_list_lock);
		kmem_cache_free(allocator, entry);
	}
	spin_unlock(&fb_list_lock);
	
	//deactivate_super(real_super);
	//blkdev = NULL;
        //real_super = NULL;
}

static void monitor_media(void)
{
	struct ext4_sb_info *sbi;
	uint64_t zblocks;
	sbi = EXT4_SB(real_super);
	zblocks = atomic64_read(&total_blocks);
	while(was_on) {
		int i;
		//int idle = 0;
		char dev_name[6];
		u64 bfree, pz_blocks;
		unsigned long read_write, zio, zfree;
                unsigned int margin = 80 * cur_num_thread;
                int need_thread = 0;
		res.MediaReads.Uint64 = 0;
		res.MediaReads.Uint64_1 = 0;
		res.MediaWrites.Uint64 = 0;
		res.MediaWrites.Uint64_1 = 0;

		for(i = 0; i < 6; i++) {
			snprintf(dev_name, 6, "nmem%d", i);
			rc = dev_nd_ioctl(dev_name, ND_IOCTL_VENDOR, 
				(unsigned long)&pcmd->cmd_buf, DIMM_IOCTL);	
		//	if(rc) 
		//		printk(KERN_ERR "%s: Error on nd_ioctl\n", __func__);
			memcpy(&output[i], tail->out_buf, sizeof(meminfo));
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
		read_bytes = res.MediaReads.Uint64 * 64 < num_freeing_blocks*4096 ?
				0 : res.MediaReads.Uint64 * 64
					- num_freeing_blocks * 4096;
		write_bytes = res.MediaWrites.Uint64 * 64 < num_freeing_blocks * 4096 ?
				0 : res.MediaWrites.Uint64*64 
					- num_freeing_blocks * 4096;
		
		zspeed_monitor = (num_freeing_blocks*4096)/(1<<20);
		//printk(KERN_ERR "before zspeed %lu\n", zspeed);
		// long long int total_time = ktime_to_ns(elapsed_time) + ktime_to_ns(elapsed_time);
		total_time = ktime_to_ns(elapsed_time);
                /*
		if(total_time > 0){
			printk(KERN_ERR "zspeed and elapsed_time: %lu MB/s %lu ns\n", zspeed_monitor, total_time);
		}
		else{
			printk(KERN_ERR "total time is 0 or negative\n");
		}
                */
		num_freeing_blocks = 0;
		elapsed_time = 0;

                if (zspeed_monitor && zspeed_monitor + margin < zspeed) {
                  need_thread = min_t(u64, (zspeed / (zspeed_monitor / cur_num_thread)) + 1, 4);
                } else if (need_shrink) {
                  need_thread = max_t(u64, cur_num_thread - 1, 1);
                }

                if (need_thread >= 1) {
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
		read_write = (10*read_bytes/25+write_bytes)/(1<<20);
		if (read_write >= 8000)
			read_write = 8000;
		
		zio = min_t(u64, 8000 - read_write, 4000);
		bfree =	percpu_counter_sum_positive(&sbi->s_freeclusters_counter)  - 
			percpu_counter_sum_positive(&sbi->s_dirtyclusters_counter);
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
		zspeed = max(zio, zfree);
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
      kthread_stop(threads[i]);
    }
  }
  else if (need_thread > cur_num_thread){
    static struct task_struct **tmp;
    //create kthread
    for (i = cur_num_thread; i < need_thread; i++) {
      tmp = &threads[i];
      *tmp = kthread_create((int(*)(void*))kt_free_block,
          NULL, "kt_free_block");
      wake_up_process(*tmp);
    }
  }

  return need_thread;
}

/*
unsigned long timer_interval_ns = 1e6; 
static struct hrtimer hr_timer;

enum hrtimer_restart timer_callback(struct hrtimer *timer_for_restart) { 
	ktime_t currtime, interval; 
	currtime = ktime_get(); 
	interval = ktime_set(0, timer_interval_ns); 
	hrtimer_forward(timer_for_restart, currtime, interval); 
	return HRTIMER_RESTART; 
}
*/

int __init kt_free_block_init(void) 
{
	int ret = 0;
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
	spin_lock_init(&kt_free_lock);
	INIT_LIST_HEAD(&block_list);
	
	
	ext4_free_data_cachep = KMEM_CACHE(ext4_free_data,
			SLAB_RECLAIM_ACCOUNT);
	allocator = kmem_cache_create("zero_waiting_block_list", sizeof(struct
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
	tmp_entry = kmem_cache_alloc(allocator, GFP_KERNEL);

	frblk_kobj = kobject_create_and_add("free_block", NULL);
	ret = sysfs_create_group(frblk_kobj, &frblk_group);
	if(ret) {
		printk("%s: sysfs_create_group() failed. ret=%d\n", __func__,
				ret);
	}

	/* Initialize nd_ioctl commands and buffer
	 * */
	size = sizeof(*pcmd) + sizeof(struct nd_cmd_vendor_hdr) 
		+ sizeof(struct nd_cmd_vendor_tail) + 128 + 128;
	ndctl_alloc = kmem_cache_create("pm_monitoring", size, 0, 0, NULL);
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

        threads[0] = thread;
        threads[1] = thread2;
        threads[2] = thread3;
        threads[3] = thread4;
	
	/*
	ktime_t ktime = ktime_set(0, timer_interval_ns); 
	printk("ktime_get_ns : %llu\n", ktime_get_ns()); 
	
	hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL); 
	hr_timer.function = &timer_callback; 
	hrtimer_start(&hr_timer, ktime, HRTIMER_MODE_REL);
	*/
	return 1;
}


void __exit kt_free_block_cleanup(void)
{
	//int ret = hrtimer_cancel(&hr_timer);
	//if (ret) printk("The timer was still in use\n");


	printk(KERN_INFO "Cleaning up kt_free_block module...\n");
	kmem_cache_free(ndctl_alloc, pcmd);
	kmem_cache_free(allocator, tmp_entry);
	kmem_cache_shrink(ext4_free_data_cachep);
        kfree(blkdev_name);
	kmem_cache_shrink(allocator);
	kmem_cache_shrink(ndctl_alloc);
}


//module_init(kt_free_block_init);
//module_exit(kt_free_block_cleanup);
