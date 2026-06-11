#ifndef DLG_H
#define DLG_H

#include "lockfree_list.h"

#define DLG_FREE	1
#define DLG_ALLOC	2

int dlg_enqueue(struct nova_sb_info *sbi, void *task, int target_bucket, int task_type);
int dlg_init(struct nova_sb_info *sbi);
void dlg_exit(struct nova_sb_info *sbi);
//extern struct kmem_cache *dlg_free_task_slab;
//extern struct kmem_cache *dlg_alloc_task_slab;

struct dlg_alloc_task {
	struct super_block *sb;
	unsigned long *blocknr;
	unsigned int num;
	unsigned short btype;
	int zero;
	enum alloc_type atype;
	int cpuid;
	enum nova_alloc_direction from_tail;

	int ret;

	struct completion comp;
};

struct dlg_free_task {
	struct super_block *sb;
	unsigned long blocknr;
	int num;
	unsigned short btype;
	int log_page;
};

struct dlg_item {
	struct list_head list;
	unsigned short task_type;

	union {
		struct dlg_alloc_task *at;
		struct dlg_free_task ft;
	} u;

	int request_cpu;
};

struct dlg_worker {
	struct task_struct *thread;
	wait_queue_head_t wq;

//	spinlock_t creation_lock;
//	struct list_head creation_q;
	struct lf_list lf_creation_q;

	spinlock_t runnable_lock;
	struct list_head runnable_q;

	struct nova_sb_info *sbi;
	int bucket;
};

struct dlg_ctx {
	struct dlg_worker *workers;
	unsigned int nbuckets;
};

#endif
