#include <linux/kernel.h>
#include <linux/export.h>
#include "lockfree_list.h"
#include <linux/hash.h>
#include <linux/sched.h>
#include <linux/fs.h>

#define mem_alloc(size) kmalloc(size, GFP_KERNEL)

static bool marked( struct item_node* node)
{
	if (!node)
		return false;
	return (unsigned long long)(node) & 0x1;
}

static struct item_node* unmark( struct item_node* node)
{
	return (struct item_node*)((unsigned long long)(node) & 0xFFFFFFFFFFFFFFFE);
}

static void rlock_node_rcu_free(struct rcu_head *head)
{
	struct item_node *node = container_of(head, struct item_node, rcu);

	kfree(node);

	// pr_debug("LFS: item_node[%d-%d] removed\n", start, end);
}

static struct list_head* DeleteNodeLH(struct item_node* lock)
{
	struct list_head* inode = NULL;
	unsigned long long _marked;
	while (true) {
		struct item_node* orig = lock->next;
		_marked = (unsigned long long)orig + 1;
		// BUG_ON(!marked((struct item_node *)_marked));
		if (!marked((struct item_node *)_marked)){
			return NULL;
		}
		if (cmpxchg(&lock->next, orig, (struct item_node*)_marked) == orig) {
			// pr_debug("LFS: item_node[%d-%d] Logically deleted!\n",
			// 		lock->start, lock->end);
			inode = lock->inode;
			break;
		}
	}
	return inode;
}


static int InsertNodeRW( struct item_node** head, struct item_node* lock, bool try)
{
	 struct item_node** prev;
	 struct item_node* cur;

restart:
	rcu_read_lock();
	prev = head;
	cur = *prev;

	while (true) {

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct item_node* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		lock->next = cur;
		if (cmpxchg(prev, cur, lock) == cur) {
			int ret = 0;
			rcu_read_unlock();
			return ret;
		}
		cur = *prev;

	}
	rcu_read_unlock();
	return -1;
}

static void
InitNode(struct item_node *node, struct list_head *inode)
{
	node->inode = inode;
	node->next = NULL;
}

int __insert_item(struct lf_list *lf_list,
	struct list_head *inode, bool try)
{
	struct item_node *in;
	int ret = 0;

	in = mem_alloc(sizeof(struct item_node));
	if (!in){
		goto free_rl;
	}
	InitNode(in, inode);
	// pr_debug("Lnode inode: %p\n", in->inode);
	do {
		ret = InsertNodeRW(&lf_list->head, in, false);
		if (try && ret < 0)
			goto free_rl;
	} while(ret);

	return 0;
free_rl:
	return -1;
}
EXPORT_SYMBOL(__insert_item);


struct list_head* __FetchHead(struct item_node** head)
{
	struct list_head* inode = NULL;
	struct item_node** prev;
	struct item_node* cur;

restart:
	rcu_read_lock();
	prev = head;
	cur = *prev;

	while (true) {
		if (!cur) {
			break;
		}

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct item_node* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		if (cur) {
			inode = DeleteNodeLH(cur);
			break;
		}

	}
	rcu_read_unlock();
	return inode;
}

struct list_head* FetchHead(struct lf_list *lf_list)
{
	if (lf_list->head == NULL)
		return NULL;
	return __FetchHead(&lf_list->head);
}
EXPORT_SYMBOL(FetchHead);

void bucket_init(struct lf_list *lf_list)
{
	lf_list->head = NULL;
}
EXPORT_SYMBOL(bucket_init);

