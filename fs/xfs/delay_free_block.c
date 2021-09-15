/*
 * Delay free block is mainly for zerooutting the blocks at the background. So
 * that the allocation does not require zeroing. 
 * */

#include "delay_free_block.h"


static struct task_struct *thread;
static struct task_struct *thread_monitoring;
static struct list_head block_list;
static struct kmem_cache* allocator;
static struct kmem_cache* ndctl_alloc;
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
static struct block_device *blkdev;
static struct super_block *real_super; 
static int zero_ratio;
static int rc;
static int kt_free_block(void);
static void monitor_media(void);
static void flush(void);

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
	return scnprintf(buf, PAGE_SIZE,
					"MediaReads_0: %lu\n"
					"MediaWrites_0: %lu\n"
					"Zero Ratio: %d\n"
					"Zero speed: %lu\n", 
			read_bytes/(1<<20), write_bytes/(1<<20),
			zero_ratio, zspeed);
}

static ssize_t frblk_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t len)
{
	struct frblk_attr *frblk = container_of(attr, struct frblk_attr, attr);
	int was_on = 0;
	if(frblk->value == 1)
		was_on = 1;

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
					printk(KERN_ERR "%s: Error on nd_ioctl(%d)\n", 
						__func__, rc);
				}
				memcpy(&init_rw[i], tail->out_buf, sizeof(meminfo));
			}
			devnum = ((259 & 0xfff) << 20) | (1 & 0xff) + 1;
			blkdev = bdget(devnum);
			real_super = get_active_super(blkdev);

			thread_monitoring =
				kthread_create((int(*)(void*))monitor_media, NULL,
						"monitor_media");
			wake_up_process(thread_monitoring);
			thread = kthread_create((int(*)(void *))kt_free_block,
						NULL, "kt_free_block");
			wake_up_process(thread);
		}
	} 
	else if (frblk->value == 0) {
		if (was_on) 
			thread_control = 0;	
	}
	else if (frblk->value == 3) {
		devnum = ((259 & 0xfff) << 20) | (1 & 0xff) + 1;
		blkdev = bdget(devnum);
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

int xfs_free_num_blocks(xfs_extlen_t len) 
{
	struct free_block_t *entry;
	int err;

	while(len){
		err = 0;
		spin_lock(&fb_list_lock);
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
			spin_unlock(&fb_list_lock);

			len -= tmp_entry->len;
			err = free_blocks(tmp_entry);
			if(err)
				printk(KERN_ERR "%s: ERROR on freeing extent\n",
					__func__);

			atomic64_sub(tmp_entry->len, &total_blocks);
			kmem_cache_free(allocator, entry);
		}
	}
	return 1;
}
EXPORT_SYMBOL(xfs_free_num_blocks);

static int kt_free_block(void)
{
	while(1) {
		if((long)atomic64_read(&total_blocks) >= 10000) {
			xfs_free_num_blocks(10000);
			num_freeing_blocks+=10000;
			if(zspeed >= 40)
				msleep(1000/(zspeed/40)-1);
		} else {
			msleep(10000);
		}
	}
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
}

static void monitor_media(void)
{
	// Get superblock from dev num
	struct xfs_mount *mp;
	uint64_t zblocks;
	mp = XFS_M(real_super);
	zblocks = atomic64_read(&total_blocks);
	while(1) {
		int i;
	//	int idle = 0;
		char dev_name[6];
		u64 bfree, pz_blocks;
		unsigned long read_write, zio, zfree;
//		printk(KERN_ERR "%s: Keep monitoring...\n", __func__);
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

		num_freeing_blocks = 0;
		
		read_write = (10*read_bytes/25+write_bytes)/(1<<20);
		if (read_write >= 8000)
			read_write = 8000;
		zio = min_t(unsigned long, 8000 - read_write, 4000);
		bfree =	percpu_counter_sum(&mp->m_fdblocks);
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
	allocator = kmem_cache_create("delay_free_block", sizeof(struct
				free_block_t), 0, 0, NULL);
	if(allocator == NULL) {
		printk(KERN_ERR "Kmem_cache create failed!!!!\n");
		return -1;
	}
	tmp_entry = kmem_cache_alloc(allocator, GFP_KERNEL);

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
	return 1;
}

static void __exit kt_free_block_cleanup(void)
{
	printk(KERN_INFO "Cleaning up kt_free_block module...\n");
	kmem_cache_free(ndctl_alloc, pcmd);
	kmem_cache_free(allocator, tmp_entry);
	kmem_cache_shrink(allocator);
	kmem_cache_shrink(ndctl_alloc);
}


module_init(kt_free_block_init);
module_exit(kt_free_block_cleanup);
