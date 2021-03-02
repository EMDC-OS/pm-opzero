#include "delay_free_inode.h"

static struct task_struct *thread;
static struct list_head block_list;
static struct kmem_cache* allocator; 
static struct kmem_cache* ext4_free_data_cachep; 
static spinlock_t iq_lock;
static unsigned long count_free_blocks;
static unsigned long num_free_blocks;
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
//void delay_iput(struct inode *inode)
//{
//	struct free_inode_t *new = kmem_cache_alloc(allocator, GFP_KERNEL);
//	if(new) 
//		new -> inode = inode;
//	spin_lock(&iq_lock);
//	list_add_tail(&new->ls, &block_list);
//	spin_unlock(&iq_lock);
//}
//EXPORT_SYMBOL(delay_iput);

/* ext4_delay_free_block
 * Delay the call for ext4_free_block
 * 
 */

void ext4_delay_free_inode(struct inode * inode, ext4_fsblk_t block, 
		unsigned long count, int flag)
{
	/* 
	 * Here we need to put the information into the list.
	 * Need to use kmem_cache_alloc
	 * */
	struct free_block_t* new = kmem_cache_alloc(allocator, GFP_KERNEL);
	if(new) {
		new -> inode = inode;
		new -> block = block;
		new -> flag = flag;
		new -> count = count;
	}

	spin_lock(&iq_lock);
	list_add_tail(&new -> ls, &block_list);
	spin_unlock(&iq_lock);

}
EXPORT_SYMBOL(ext4_delay_free_inode);

static int free_blocks(struct free_block_t *entry)
{
	struct inode *inode = entry -> inode;
	ext4_fsblk_t block = entry -> block;
	unsigned long count = entry -> count;
	int flags = flags;
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
	unsigned int credits;
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

	if (in_range(ext4_block_bitmap(sb, gdp), block, count) ||
			in_range(ext4_inode_bitmap(sb, gdp), block, count) ||
			in_range(block, ext4_inode_table(sb, gdp),
				sbi->s_itb_per_group) ||
			in_range(block + count - 1, ext4_inode_table(sb, gdp),
				sbi->s_itb_per_group)) {

		ext4_error(sb, "Freeing blocks in system zone - "
				"Block = %llu, count = %lu", block, count);
		/* err = 0. ext4_std_error should be a no op */
		goto error_return;
	}

	/* Modified for zeroout data blocks while trucate for dax
	 * */
	if (IS_DAX(inode)) {
		/* use iomap_zero_range need to find from and length */
		struct iomap dax_iomap, srcmap;
		loff_t written;
		dax_iomap.addr = block << inode->i_blkbits;
		dax_iomap.offset = 0;
		dax_iomap.bdev = inode -> i_sb -> s_bdev;
		dax_iomap.dax_dev = EXT4_SB(inode -> i_sb)->s_daxdev;
		srcmap.type = 2;

		written = iomap_zero_range_actor(inode, 0, inode->i_sb->s_blocksize*count, 
				NULL, &dax_iomap, &srcmap);
	}

	/* New handle for journaling
	 * */
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		credits = ext4_writepage_trans_blocks(inode);
	else
		credits = ext4_blocks_for_truncate(inode);
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);

	BUFFER_TRACE(bitmap_bh, "getting write access");
	err = ext4_journal_get_write_access(handle, bitmap_bh);
	if (err)
		goto error_return;

	/*
	 * We are about to modify some metadata.  Call the journal APIs
	 * to unshare ->b_data if a currently-committing transaction is
	 * using it
	 */
	BUFFER_TRACE(gd_bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, gd_bh);
	if (err)
		goto error_return;
#ifdef AGGRESSIVE_CHECK
	{
		int i;
		for (i = 0; i < count_clusters; i++)
			BUG_ON(!mb_test_bit(bit + i, bitmap_bh->b_data));
	}
#endif
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
	if (ext4_handle_valid(handle) &&
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
			dquot_free_block(inode, EXT4_C2B(sbi, count_clusters));
		percpu_counter_add(&sbi->s_freeclusters_counter,
				count_clusters);
	}

	ext4_mb_unload_buddy(&e4b);

	/* We dirtied the bitmap block */
	BUFFER_TRACE(bitmap_bh, "dirtied bitmap block");
	err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);

	/* And the group descriptor block */
	BUFFER_TRACE(gd_bh, "dirtied group descriptor block");
	ret = ext4_handle_dirty_metadata(handle, NULL, gd_bh);
	if (!err)
		err = ret;

	ext4_journal_stop(handle);

	if (overflow && !err) {
		block += count;
		count = overflow;
		put_bh(bitmap_bh);
		goto do_more;
	}
error_return:
	brelse(bitmap_bh);
	ext4_std_error(sb, err);
	return 0;
}
static int kt_free_inode(void)
{
	thread_running = 1;
	while(thread_control) {
		/* Do rest of the free blocks */			
		struct free_block_t *entry;
		int err;
		spin_lock(&iq_lock);
		while (!list_empty(&block_list)) {
			spin_unlock(&iq_lock);
			err = 0;
			spin_lock(&iq_lock);
			entry = list_first_entry(&block_list,
					struct free_inode_t, ls);
			spin_unlock(&iq_lock);
			if (entry -> inode == NULL) {
				spin_lock(&iq_lock);
				list_del(&entry -> ls);
				spin_unlock(&iq_lock);
				kmem_cache_free(allocator, entry);
				spin_lock(&iq_lock);
				continue;
			}
			num_free_blocks += entry->count;
			err = free_blocks(entry);
			if (err) {
				printk(KERN_ERR "Free blocks error!!!!\n");
			}

			spin_lock(&iq_lock);
			list_del(&entry -> ls);
			spin_unlock(&iq_lock);

			kmem_cache_free(allocator, entry);
			count_free_blocks++;

			/* This lock is for loop condition check */
			spin_lock(&iq_lock);
		}
		spin_unlock(&iq_lock);
		msleep(100);
	}
	thread_running = 0;
	return 0;
}

static int __init kt_free_inode_init(void) 
{
	int ret = 0;
	thread_control = 0;
	thread_running = 0;
	count_free_blocks = 0;
	num_free_blocks = 0;
	spin_lock_init(&iq_lock);
	INIT_LIST_HEAD(&block_list);
	ext4_free_data_cahcep = KMEM_CACHE(ext4_free_data,
			SLAB_RECLAIM_ACCOUNT);
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
	kmem_cache_shrink(ext4_free_data_cachep);
}


module_init(kt_free_inode_init);
module_exit(kt_free_inode_cleanup);
