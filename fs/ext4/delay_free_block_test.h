#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/slab_def.h>
#include <linux/export.h>
#include <asm/spinlock.h>
#include <linux/semaphore.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/iomap.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
#include <linux/iomap.h>
#include <linux/dax.h>
#include "../../drivers/nvdimm/nd.h"
#include "ext4.h"
#include "ext4_jbd2.h"
#include "mballoc.h"
#include "truncate.h"

noinline_for_stack int ext4_mb_load_buddy_gfp(struct super_block *sb,
		ext4_group_t group, struct ext4_buddy *e4b, gfp_t gfp);
void ext4_mb_unload_buddy(struct ext4_buddy *e4b);
void mb_free_blocks(struct inode *inode, struct ext4_buddy *e4b,
		int first, int count);
void mb_clear_bits(void *bm, int cur, int len);
inline int ext4_issue_discard(struct super_block *sb,
		ext4_group_t block_group, ext4_grpblk_t cluster, int count,
		struct bio **biop);
noinline_for_stack int ext4_mb_free_metadata(handle_t *handle,
		struct ext4_buddy *e4b, struct ext4_free_data *new_entry);

typedef struct {
	uint64_t Uint64;
	uint64_t Uint64_1;
} UINT128;

typedef uint8_t UINT8;

typedef struct {
	UINT128 MediaReads;
	UINT128 MediaWrites;
	UINT128 ReadRequests;
	UINT128 WriteRequests;
	UINT8 Reserved[64];
} meminfo;

typedef struct {
	UINT8 MemoryPage;
	UINT8 Reserved[127];
} input_info;

typedef struct { 
	union {
		meminfo test[0];
		struct nd_cmd_vendor_hdr vendor[0];
		char cmd_buf[0];
	};
} cmd;

struct ndctl_cmd { 
	int refcount;
	int type;
	int size;
	int status;
	union {
		struct nd_cmd_get_config_size get_size[0];
		struct nd_cmd_get_config_data_hdr get_data[0];
		struct nd_cmd_set_config_hdr set_data[0];
		struct nd_cmd_vendor_hdr vendor[0];
		char cmd_buf[0];
	};
};

struct free_block_t {
	struct inode *inode;
	ext4_fsblk_t block;
	int flag;
	unsigned long count; 
	struct list_head ls;
};
