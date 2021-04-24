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

#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_shared.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_extfree_item.h"
#include "xfs_log.h"
#include "xfs_btree.h"
#include "xfs_rmap.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_trace.h"
#include "xfs_error.h"
#include "xfs_log_priv.h"
#include "xfs_log_recover.h"

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
	struct xfs_mount	*mp;
	xfs_fsblock_t	start_block;
	xfs_extlen_t	len;
	uint64_t		oi_owner;
	xfs_fileoff_t	oi_offset;
	unsigned int	oi_flags;
	bool			skip_discard;
	struct list_head ls;
};
