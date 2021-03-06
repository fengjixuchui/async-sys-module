/*
 * Copyright (c) 2017 Sean Wilson <spwilson2@wisc.edu>
 *
 * This file is released under the GPLv2
 */

#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/file.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/pid.h>
#include <linux/stddef.h>

#include "buffer.h"
#include "common.h"

// TODO: Create global map for id->void* of allocated buffers.
struct {
	rwlock_t lock;

	struct rb_root _root;
	buffer_id_t _uid_counter;
} map_wrapper = {
	RW_DEP_MAP_INIT(map_wrapper.lock)
	._root = {NULL},
	._uid_counter = 0
};

static inline buffer_id_t gen_next_map_id(void)
{
	/* FIXME: Make atomic. */
	return map_wrapper._uid_counter++;
}

struct map_entry {
	struct rb_node node;
	struct map_key key;
	struct buffer_slab buffer;
};

/* A linked list of pointers to map enteries. */
struct map_entry_list {
	struct map_entry *entry;
	struct list_head list;
};

struct file_ll_node {
	struct map_key map_key;
	struct list_head list;
};

struct file_ll_head {
	struct spinlock spinlock;
	struct list_head list;
};

struct kernel_data {
	struct map_entry map_entry;
	struct file_ll_node file_ll_node;
	char kernel_buffer[0]; // Variable length attr.
};

static inline int
map_key_cmp(struct map_key *l_key, struct map_key *r_key)
{
	if (l_key->pid < r_key->pid)
		return -1;
	else if (l_key->pid > r_key->pid)
		return 1;
	else {
		if (l_key->buffer_uid < r_key->buffer_uid)
			return -1;
		else if (l_key->buffer_uid > r_key->buffer_uid)
			return 1;
		else
			return 0;
	}
};

/*
 * See linux/Documentation/rbtree.txt for original source.
 */
static struct map_entry *
map_search(struct rb_root *root, struct map_key *key)
{
	struct rb_node *node = root->rb_node;

	/* Traverse the tree looking for our key */
	while (node) {
		struct map_entry *data = container_of(node, struct map_entry, node);
		int result = map_key_cmp(key, &data->key);
		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

/**
 * map_search_pid() - Search the map for all enteries with pid
 *
 * Return:		A linked list of them of all entries with a given pid.
 */
//static struct map_entry_list *
//map_search_pid(struct rb_root *root, pid_t pid)
//{
//	// TODO: This will be needed for deletion of an entire file's enteries.
//	return NULL;
//}

/*
 * See linux/Documentation/rbtree.txt for original source.
 */
static int
map_insert(struct rb_root *root, struct map_entry *data)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct map_entry *this = container_of(*new, struct map_entry, node);
		int result = map_key_cmp(&data->key, &this->key);

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0 )
			new = &((*new)->rb_right);
		else
			return false;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);

	return true;
}

/**
 * alloc_buffer() - Allocate a buffer for a given file.
 * @size		Size of the buffer in bytes to allocate.
 * @file		The file context which we are going to
 *			attach the given buffer into.
 *
 * In order to maintain state information for processes the
 * file will use the file.private_data pointer to hold a unique
 * map key id for each buffer it holds. The format for this
 * list of map key ids for a file will be a singly linked list
 * to allow quick removal and additions.
 *
 */
int
alloc_buffer(size_t user_buffer_size, size_t kernel_buffer_size,
		struct file *file, struct buffer_slab **buffer)
{

	struct kernel_data *kernel_data;

	/* Allocate space for the map entry*/
	kernel_data = kmalloc(sizeof(struct kernel_data) + kernel_buffer_size, GFP_KERNEL);
	if (!kernel_data) {
		// TODO: Need to try a vmalloc if unable to succeed.
		return false; // Failed to alloc.
	}
	kernel_data->map_entry.buffer.kernel_buffer = &kernel_data->kernel_buffer;

	/* Allocate space for our shared ring buffer. */
	kernel_data->map_entry.buffer.user_buffer = kmalloc(user_buffer_size, GFP_USER);
	if (!kernel_data->map_entry.buffer.user_buffer) {
		// TODO: Need to try a vmalloc if unable to succeed.
		kfree(kernel_data);
		return false; // Failed to alloc.
	}
	/*
	 * TODO: Need to set up the address space boundaries for the correct
	 * process for the allocated buffer.
	 */

	// Initilize the lock on the new map_entry's buffer and grab the lock.
	rwlock_init(&kernel_data->map_entry.buffer.rwlock);
	write_lock(&kernel_data->map_entry.buffer.rwlock);

	read_lock(&file->f_owner.lock);
	kernel_data->map_entry.key = (struct map_key){.buffer_uid = gen_next_map_id(), .pid = pid_nr(file->f_owner.pid)};

	/* Grab the lock on this file's list of buffer entries. */
	spin_lock(&((struct file_ll_head*)file->private_data)->spinlock);

	write_lock(&map_wrapper.lock);
	read_unlock(&file->f_owner.lock);
	if (!map_insert(&map_wrapper._root, &kernel_data->map_entry)) {
		// There was a duplicate....?
		write_unlock(&map_wrapper.lock);
		read_unlock(&file->f_owner.lock);

		kfree(kernel_data->map_entry.buffer.user_buffer);
		kfree(kernel_data);
		return false;
	}
	*buffer = &kernel_data->map_entry.buffer;
	write_unlock(&map_wrapper.lock);

	/* With the node inserted into the tree we can now insert a tag into
	 * the file's list of active buffers.
	 */
	list_add(&kernel_data->file_ll_node.list, &((struct file_ll_head*)file->private_data)->list);
	spin_unlock(&((struct file_ll_head*)file->private_data)->spinlock);

	return true;
}

/**
 * free_buffer() - Free the buffer of given id
 */
void
free_buffer(buffer_id_t id, struct file *file)
{
	struct map_key map_key;
	struct map_entry *match;
	struct kernel_data *kernel_data;

	read_lock(&file->f_owner.lock);
	map_key = (struct map_key){.buffer_uid = id, .pid = pid_nr(file->f_owner.pid)};
	read_unlock(&file->f_owner.lock);

	write_lock(&map_wrapper.lock);
	if (!(match = map_search(&map_wrapper._root, &map_key))) {
		// No match found
		write_unlock(&map_wrapper.lock);
		mpr_err("Called free_buffer for id '%lu' but no match found.", id);
		return;
	}

	kernel_data = container_of(match, struct kernel_data, map_entry);
	spin_lock(&((struct file_ll_head*)file->private_data)->spinlock);

	/* Note: The following order matters! */
	/* Grab lock and never free since we free it */
	write_lock(&match->buffer.rwlock);

	/* Remove the mapping from the tree. */
	rb_erase(&match->node, &map_wrapper._root);
	/* Remove this linked list node from the list. */
	list_del(&kernel_data->file_ll_node.list);
	/* We can now unlock the tree since there is no way to find the buffer */
	write_unlock(&map_wrapper.lock);
	spin_unlock(&((struct file_ll_head*)file->private_data)->spinlock);

	kfree(match->buffer.user_buffer);
	kfree(kernel_data);
}

/* Get the buffer from the map. */
int
get_buffer(buffer_id_t id, pid_t pid, struct buffer_slab **buffer) {
	struct map_key map_key = (struct map_key){.buffer_uid = id, .pid = pid};
	struct map_entry *match;

	read_lock(&map_wrapper.lock);
	if (!(match = map_search(&map_wrapper._root, &map_key))) {
		// No match found
		read_unlock(&map_wrapper.lock);
		return false;
	}
	*buffer = &match->buffer;
	/* Hand-over-hand lock must be done. */
	read_lock(&match->buffer.rwlock);
	read_unlock(&map_wrapper.lock);
	return true;
}

int
buffer_init_file(struct file *file)
{
	struct file_ll_head* new_ll;
	void * volatile*private_data = &file->private_data;

	if (!(new_ll = kmalloc(sizeof(struct file_ll_head), GFP_KERNEL)))
		return false;

	INIT_LIST_HEAD(&new_ll->list);
	spin_lock_init(&new_ll->spinlock);

	/*
	 * This must occur after the spin_lock_init hence the volatile decl
	 * above.
	 */
	*private_data = new_ll;
	return true;
}

void
buffer_free_file(struct file *file)
{
	struct kernel_data *kernel_data;
	struct file_ll_node *cur_node, *next_node;

	/*
	 * FIXME: Race condition if close is called twice. Could crash if try to
	 * access private data with a double free. Fix would be some kind of
	 * lock to prevent private_data from disappearing between this null
	 * check and grabbing its spinlock.
	 */
	if (!file->private_data)
		return;

	spin_lock(&((struct file_ll_head*)file->private_data)->spinlock);
	write_lock(&map_wrapper.lock);

	/*
	 * Iterate over the linked list of buffer entries associated with this
	 * file deleting the enteries and then freeing their buffers.
	 */
	list_for_each_entry_safe(cur_node,
				 next_node,
				 &((struct file_ll_head*)file->private_data)->list,
				 list) {

		kernel_data = container_of(cur_node, struct kernel_data, file_ll_node);
		/* Grab the write lock for the ability to delete this buffer. */
		write_lock(&kernel_data->map_entry.buffer.rwlock);
		/* Remove the mapping from the tree. */
		rb_erase(&kernel_data->map_entry.node, &map_wrapper._root);
		/* Remove this linked list node from the list. */
		list_del(&kernel_data->file_ll_node.list);
		kfree(kernel_data->map_entry.buffer.user_buffer);
		kfree(kernel_data);
	}
	write_unlock(&map_wrapper.lock);
	spin_unlock(&((struct file_ll_head*)file->private_data)->spinlock);
}
