#ifndef LOCKFREE_LIST_H
#define LOCKFREE_LIST_H

#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#define MAX_SIZE (0xFFFFFFFF)
#define ALL_RANGE (0xFFFFFFFF)

struct item_node {
	struct list_head *inode;
	 struct item_node* next;
	struct rcu_head rcu;
};

struct lf_list {
	 struct item_node* head;
};

struct list_head* FetchHead(struct lf_list *lf_list);

int __insert_item(struct lf_list *lf_list, struct list_head *inode, bool try);

static inline int insert_item(struct lf_list* lf_list,
		struct list_head *inode, bool writer)
{
	return __insert_item(lf_list, inode, false);
}

void bucket_init(struct lf_list *lf_list);

#endif /* LOCKFREE_LIST_H */
