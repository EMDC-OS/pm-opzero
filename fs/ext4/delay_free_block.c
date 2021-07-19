/*
 * Delay free block is mainly for zerooutting the blocks at the background. So
 * that the allocation does not require zeroing. 
 * */

#include "delay_free_block_test.h"


static struct task_struct *thread;
static struct task_struct *thread_monitoring;
static struct list_head block_list;
static struct kmem_cache* allocator;
static struct kmem_cache* ndctl_alloc;
static struct kmem_cache* ext4_free_data_cachep;
static spinlock_t fb_list_lock; 
static unsigned long count_free_blocks;
static unsigned long num_free_blocks;
static unsigned long num_freeing_blocks;
static atomic64_t total_blocks;
static unsigned long read_bytes, write_bytes;
static unsigned long zspeed;
static int thread_control;
static struct ndctl_cmd *pcmd;
static meminfo output[6];
static meminfo res;
static meminfo init_rw[6];
static input_info input;
static struct nd_cmd_vendor_tail *tail;
static dev_t devnum;
static struct free_block_t *tmp_entry;
static int zero_ratio, threshold;
static int rc;
static int kt_free_block(void);
static void monitor_media(void);
static void flush(void);
void ext4_delay_free_block(struct inode * inode, ext4_fsblk_t block, 
		unsigned long count, int flag);

static struct kobject *frblk_kobj;

struct frblk_attr{
	struct kobj_attribute attr;
	int value;
};

static struct frblk_attr frblk_value;
static struct frblk_attr frblk_notify;

static struct attribute *frblk_attrs[] = {
	&frblk_value.attr.attr,
	&frblk_notify.attr.attr,
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
					"RC: %d\n"
					"Zero Ratio: %d\n"
					"Zero_speed: %lu",
			read_bytes, write_bytes,
			rc, zero_ratio, zspeed);
}

static ssize_t frblk_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	struct frblk_attr *frblk = container_of(attr, struct frblk_attr, attr);
	int was_on = 0;
	if(frblk->value)
		was_on = 1;

	sscanf(buf, "%d", &frblk->value);
	sysfs_notify(frblk_kobj, NULL, "frblk_notify");
	if (frblk->value == 1) {
		if (was_on == 0) {
			int i;
		//	struct inode * asdf = 0x0000000080550038;
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
		//	ext4_delay_free_block(asdf, 1000, 10000, 0);
			thread_monitoring =
				kthread_create((int(*)(void*))monitor_media, NULL,
						"monitor_media");
			wake_up_process(thread_monitoring);
			thread = kthread_create((int(*)(void*))kt_free_block,
					NULL, "kt_free_block");
			wake_up_process(thread);

		}
	} 
	else if (frblk->value == 0) {
		if (was_on) 
			thread_control = 0;	
	}
	else if (frblk->value == 3) {
		flush();
	}
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
	struct free_block_t *new = kmem_cache_alloc(allocator, GFP_KERNEL);
	if(new) {
		new -> inode = inode;
		new -> block = block;
		new -> flag = flag;
		new -> count = count;
	}

	spin_lock(&fb_list_lock);
	list_add_tail(&new -> ls, &block_list);
	spin_unlock(&fb_list_lock);

	atomic64_add(count, &total_blocks);
}
EXPORT_SYMBOL(ext4_delay_free_block);

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
	struct super_block *sb = inode -> i_sb; 
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
	if (IS_DAX(inode)) {
		/* use iomap_zero_range need to find from and length */
		struct iomap dax_iomap, srcmap;
		loff_t written = 0;
		dax_iomap.addr = block << inode->i_blkbits;
		dax_iomap.offset = 0;
		dax_iomap.bdev = inode -> i_sb -> s_bdev;
		dax_iomap.dax_dev = EXT4_SB(inode -> i_sb)->s_daxdev;
		srcmap.type = 2;
		written = iomap_zero_range_actor(inode, 0, 
					inode->i_sb->s_blocksize*count, 
					NULL, &dax_iomap, &srcmap);
	}

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
			spin_unlock(&fb_list_lock);

			count -= tmp_entry->count;
			err = free_blocks(tmp_entry);
			
			atomic64_sub(tmp_entry->count, &total_blocks);

			kmem_cache_free(allocator, entry);
		}
	}
	return 1;
}
EXPORT_SYMBOL(ext4_free_num_blocks);

static int kt_free_block(void)
{
	while(1) {
		if((long)atomic64_read(&total_blocks) >= 10000) {
			ext4_free_num_blocks(10000);
			num_freeing_blocks += 10000;
			if(zspeed >= 40)
				msleep(1000/(zspeed/40)-1);
		} else {
			msleep(10000);
		}
	}
	/*
	while(thread_control) {
		//
		struct free_block_t *entry;
		int err;
		spin_lock(&fb_list_lock);
		while (!list_empty(&block_list) && thread_control) {
			err = 0;
			entry = list_first_entry(&block_list,
					struct free_block_t, ls);
			if (entry -> inode == NULL) {
				list_del(&entry -> ls);
				spin_unlock(&fb_list_lock);

				atomic64_sub(entry->count, &total_blocks);

				kmem_cache_free(allocator, entry);
				spin_lock(&fb_list_lock);
				continue;
			}
			list_del(&entry -> ls);
			spin_unlock(&fb_list_lock);

			num_freeing_blocks += entry->count;
			err = free_blocks(entry);
			num_free_blocks += entry->count;
			
			atomic64_sub(entry->count, &total_blocks);
			kmem_cache_free(allocator, entry);
			count_free_blocks++;

		//
			spin_lock(&fb_list_lock);
		}
		spin_unlock(&fb_list_lock);
		msleep(1000);
	}*/
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
			list_del(&entry -> ls);
			spin_unlock(&fb_list_lock);
			atomic64_sub(entry-> count, &total_blocks);
			kmem_cache_free(allocator, entry);
			spin_lock(&fb_list_lock);
			continue;
		}
		list_del(&entry -> ls);
		spin_unlock(&fb_list_lock);

		num_freeing_blocks += entry->count;
		err = free_blocks(entry);
		num_free_blocks += entry->count;
		
		atomic64_sub(entry->count, &total_blocks);
		kmem_cache_free(allocator, entry);
		count_free_blocks++;

		spin_lock(&fb_list_lock);
	}
	spin_unlock(&fb_list_lock);
}

static void monitor_media(void)
{
	struct block_device *blkdev;
	struct super_block *real_super;
	struct ext4_sb_info *sbi;
	uint64_t zblocks;
	devnum = ((259 & 0xfff) << 20) | (1 & 0xff) - 1;
	blkdev = bdget(devnum);
	real_super = get_active_super(blkdev);
	if(!real_super) {
		printk(KERN_ERR "%s: Super block NULL pointer\n", __func__);
		return;
	}
	sbi = EXT4_SB(real_super);
	zblocks = atomic64_read(&total_blocks);
	while(1) {
		int i;
		//int idle = 0;
		char dev_name[6];
		u64 bfree, pz_blocks;
		unsigned long read_write, zio, zfree;
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
		num_freeing_blocks = 0;
	
		read_write = (10*read_bytes/25+write_bytes)/(1<<20);
		
		zio = 8000 - read_write;
		
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
		msleep(1000);
		/*
		if (read_bytes <= 10*1024*1024 && write_bytes <= 10*1024*1024)
			idle = 1;

		bfree =	percpu_counter_sum_positive(&sbi->s_freeclusters_counter)  - 
			percpu_counter_sum_positive(&sbi->s_dirtyclusters_counter);
		pz_blocks = (u64) atomic64_read(&total_blocks);
		zero_ratio = 100 * pz_blocks / ( bfree + pz_blocks );
		if (pz_blocks - zblocks > 0) {
			threshold = min(100 * write_bytes / ((pz_blocks - zblocks)*4096), 
					(uint64_t)99);
		} else {
			goto period_control;
		}
		zblocks = pz_blocks;
	

		if(zero_ratio > threshold || idle ) {
			 //We should wake up free_block thread when idle
			 // and disk gets near full
			 
			if(!thread_control) {
				thread_control = 1;
				thread = kthread_create((int(*)(void*))kt_free_block,
						NULL, "kt_free_block");
				wake_up_process(thread);
			}
		}
		else {
			thread_control = 0;
		}

period_control:
		//Period controller, when idle -1 second for each time at max of
		//10. +1 for non-idle time
		period_control = idle ? period_control-1 : period_control+1;
		if(idle) 
			period_control = max(period_control, 1);
		else
			period_control = min(period_control, 10);
		msleep(1000 * period_control);
		*/
	}
}

static int __init kt_free_block_init(void) 
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
	INIT_LIST_HEAD(&block_list);
	ext4_free_data_cachep = KMEM_CACHE(ext4_free_data,
			SLAB_RECLAIM_ACCOUNT);
	allocator = kmem_cache_create("zero_waiting_block_list", sizeof(struct
				free_block_t), 0, 0, NULL);
	if(allocator == NULL) {
		printk(KERN_ERR "Kmem_cache create failed!!!!\n");
		return -1;
	}
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
	return 1;
}

static void __exit kt_free_block_cleanup(void)
{
	printk(KERN_INFO "Cleaning up kt_free_block module...\n");
	kmem_cache_free(ndctl_alloc, pcmd);
	kmem_cache_free(allocator, tmp_entry);
	kmem_cache_shrink(ext4_free_data_cachep);
	kmem_cache_shrink(allocator);
	kmem_cache_shrink(ndctl_alloc);
}


module_init(kt_free_block_init);
module_exit(kt_free_block_cleanup);
