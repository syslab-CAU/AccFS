#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include "nova.h"
#include <linux/fs.h>
#include "dlg.h"


//static struct kmem_cache *dlg_slab;
//struct kmem_cache *dlg_free_task_slab;
//struct kmem_cache *dlg_alloc_task_slab;

#define MAX_TASK 10000

int nova_free_blocks_internal(struct super_block *sb, unsigned long blocknr,
	int num, unsigned short btype, int log_page);

int nova_new_blocks_internal(struct super_block *sb, unsigned long *blocknr,
	unsigned int num, unsigned short btype, int zero,
	enum alloc_type atype, int cpuid, enum nova_alloc_direction from_tail);

static inline struct dlg_item *dlg_creation_pop(struct dlg_worker *wk)
{
	struct list_head *lh = FetchHead(&wk->lf_creation_q);
	if (!lh)
		return NULL;

	return container_of(lh, struct dlg_item, list);
}

static inline void dlg_creation_push(struct dlg_worker *wk, struct dlg_item *it)
{
	insert_item(&wk->lf_creation_q, &it->list, false);
}

static int dlg_thread(void *data)
{
	struct dlg_worker *wk = data;
	struct nova_sb_info *sbi = wk->sbi;

	unsigned short task_type;

	for (;;) {
		wait_event_interruptible(wk->wq, 
				!list_empty(&wk->runnable_q) || kthread_should_stop());

		spin_lock(&wk->runnable_lock);
		if (kthread_should_stop() && list_empty(&wk->runnable_q)) {
			spin_unlock(&wk->runnable_lock);
			break;
		}

		while (!list_empty(&wk->runnable_q)) {
			struct dlg_item *it =
				list_first_entry(&wk->runnable_q, struct dlg_item, list);

			list_del (&it->list);
			spin_unlock(&wk->runnable_lock);

			task_type = it->task_type;
			if (task_type == DLG_FREE) {
				struct dlg_free_task *ft = &it->u.ft;
				nova_free_blocks_internal(ft->sb, ft->blocknr, 
						ft->num, ft->btype, ft->log_page);

				struct dlg_worker *request_wk = &sbi->dlg_ctx->workers[it->request_cpu];
//				spin_lock(&request_wk->creation_lock);
//				list_add_tail(&it->list, &request_wk->creation_q);
//				spin_unlock(&request_wk->creation_lock);

				dlg_creation_push(request_wk, it);

//				kmem_cache_free(dlg_free_task_slab, ft);
			} else if (task_type == DLG_ALLOC) {
				struct dlg_alloc_task *at = it->u.at;
				at->ret = nova_new_blocks_internal(at->sb, at->blocknr, at->num, at->btype,
						at->zero, at->atype, at->cpuid, at->from_tail);

				complete(&at->comp);
			}

//			kmem_cache_free(dlg_slab, it);

			spin_lock(&wk->runnable_lock);
		}
		spin_unlock(&wk->runnable_lock);
	}
	
	return 0;
}

atomic_t tmp_cnt;
int dlg_enqueue(struct nova_sb_info *sbi, void *task, int target_bucket, int task_type)
{
	struct dlg_worker *target_wk, *my_wk;
	struct dlg_item *it;

	int my_cpu = smp_processor_id();

	my_wk = &sbi->dlg_ctx->workers[my_cpu];
	target_wk = &sbi->dlg_ctx->workers[target_bucket];
		
//	it = kmem_cache_zalloc(dlg_slab, GFP_KERNEL);
//	if (!it) {
//		printk(KERN_ERR "[%s] it=NULL\n", __func__);
//		return -ENOMEM;
//	}

//	for (;;) {
//		spin_lock(&my_wk->creation_lock);
//
//		if (!list_empty(&my_wk->creation_q)) {
//			break;
//		}
//		printk("[%s] roop!!!!!!!!!\n", __func__);
//		spin_unlock(&my_wk->creation_lock);
//	}
//
//	it = list_first_entry(&my_wk->creation_q, struct dlg_item, list);
//	list_del(&it->list);
//	spin_unlock(&my_wk->creation_lock);


	for (;;) {
		//atomic_inc(&tmp_cnt);
		it = dlg_creation_pop(my_wk);
		if (it) 
			break;
	
//		printk("[%s] roop!!!!!!!!!\n", __func__);
		cpu_relax();
		cond_resched();
	}

	it->task_type = task_type;

	if (it->task_type == DLG_FREE) {
		struct dlg_free_task *ft = task;
		it->u.ft.sb = ft->sb;
		it->u.ft.blocknr = ft->blocknr;
		it->u.ft.num = ft->num;
		it->u.ft.btype = ft->btype;
		it->u.ft.log_page = ft->log_page;
	} else if (it->task_type == DLG_ALLOC) {
		it->u.at = task;
	}

	it->request_cpu = my_cpu;

	spin_lock(&target_wk->runnable_lock);
	list_add_tail(&it->list, &target_wk->runnable_q);
	spin_unlock(&target_wk->runnable_lock);
	wake_up_interruptible(&target_wk->wq);

	if (it->task_type == DLG_FREE) {
		return 0;
	} else if (it->task_type == DLG_ALLOC) {
		wait_for_completion(&it->u.at->comp);

//		spin_lock(&my_wk->creation_lock);
//		list_add_tail(&it->list, &my_wk->creation_q);
//		spin_unlock(&my_wk->creation_lock);
		dlg_creation_push(my_wk, it);
	}

	return 0;
}

int dlg_init(struct nova_sb_info *sbi)
{
	printk("[%s]: start\n", __func__);
	unsigned int i, nb = num_online_cpus();
	struct dlg_ctx *ctx;

//	if (!dlg_slab) {
//		dlg_slab = kmem_cache_create("dlg_item", sizeof(struct dlg_item),
//				0, SLAB_RECLAIM_ACCOUNT, NULL);
//		if (!dlg_slab)
//			return -ENOMEM;
//	}

//	if (!dlg_free_task_slab) {
//		dlg_free_task_slab = kmem_cache_create("dlg_free_task_item", sizeof(struct dlg_free_task),
//				0, SLAB_RECLAIM_ACCOUNT, NULL);
//		if (!dlg_free_task_slab)
//			return -ENOMEM;
//	}
//
//	if (!dlg_alloc_task_slab) {
//		dlg_alloc_task_slab = kmem_cache_create("dlg_alloc_task_item", sizeof(struct dlg_alloc_task),
//				0, SLAB_RECLAIM_ACCOUNT, NULL);
//		if (!dlg_alloc_task_slab)
//			return -ENOMEM;
//	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->nbuckets = nb;
	ctx->workers = kcalloc(nb, sizeof(*ctx->workers), GFP_KERNEL);
	if (!ctx->workers) {
		kfree(ctx);
		return -ENOMEM;
	}

	for (i = 0; i < nb; i++) {
		struct dlg_worker *wk = &ctx->workers[i];
		wk->sbi = sbi;
		wk->bucket = i;
		init_waitqueue_head(&wk->wq);
		spin_lock_init(&wk->runnable_lock);
		INIT_LIST_HEAD(&wk->runnable_q);

//		spin_lock_init(&wk->creation_lock);
//		INIT_LIST_HEAD(&wk->creation_q);

		bucket_init(&wk->lf_creation_q);

		int j;
		for (j = 0; j < MAX_TASK; j++) {
			struct dlg_item *it;
			it = kzalloc(sizeof(struct dlg_item), GFP_KERNEL);
			
//			spin_lock(&wk->creation_lock);
//			list_add_tail(&it->list, &wk->creation_q);
//			spin_unlock(&wk->creation_lock);
			dlg_creation_push(wk, it);
		}


		wk->thread = kthread_create(dlg_thread, wk, "dlg/%u", i);
		if (IS_ERR(wk->thread)) {
			int err = PTR_ERR(wk->thread);

			while (i--)
				kthread_stop(ctx->workers[i].thread);

			kfree(ctx->workers);
			kfree(ctx);
			return err;
		}

		kthread_bind(wk->thread, i);
		wake_up_process(wk->thread);
	}
	
	sbi->dlg_ctx = ctx;
	printk("[%s]: end\n", __func__);
	return 0;
}

void dlg_exit(struct nova_sb_info *sbi)
{
	printk("[%s]: start\n", __func__);
	unsigned int i;
	
	struct dlg_ctx *ctx = sbi->dlg_ctx;
	if (!ctx) return;

	for (i = 0; i < ctx->nbuckets; i++) {
		struct dlg_worker *wk = &ctx->workers[i];
		kthread_stop(wk->thread);
		wake_up(&wk->wq);

		spin_lock(&wk->runnable_lock);
		BUG_ON(!list_empty(&wk->runnable_q));
		spin_unlock(&wk->runnable_lock);

	}

	for (i = 0; i < ctx->nbuckets; i++) {
		struct dlg_worker *wk = &ctx->workers[i];
//		struct dlg_item *cur, *tmp;
//		list_for_each_entry_safe(cur, tmp, &wk->creation_q, list) {
//			list_del(&cur->list);
//			kfree(cur);
//		}
		struct dlg_item *it;
		while ((it = dlg_creation_pop(wk)) != NULL) {
			kfree(it);
		}
	}

	kfree(ctx->workers);
	kfree(ctx);
	sbi->dlg_ctx = NULL;

//	if (dlg_free_task_slab) {
//		kmem_cache_destroy(dlg_free_task_slab);
//		dlg_free_task_slab = NULL;
//	}
//
//	if (dlg_alloc_task_slab) {
//		kmem_cache_destroy(dlg_alloc_task_slab);
//		dlg_alloc_task_slab = NULL;
//	}

//	if (dlg_slab) {
//		kmem_cache_destroy(dlg_slab);
//		dlg_slab = NULL;
//	}
	printk("[%s]: end\n", __func__);
}










