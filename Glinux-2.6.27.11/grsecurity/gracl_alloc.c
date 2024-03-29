#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/gracl.h>
#include <linux/grsecurity.h>

static unsigned long alloc_stack_next = 1;
static unsigned long alloc_stack_size = 1;
static void **alloc_stack;

static __inline__ int
alloc_pop(void)
{
	if (alloc_stack_next == 1)
		return 0;

	kfree(alloc_stack[alloc_stack_next - 2]);

	alloc_stack_next--;

	return 1;
}

static __inline__ void
alloc_push(void *buf)
{
	if (alloc_stack_next >= alloc_stack_size)
		BUG();

	alloc_stack[alloc_stack_next - 1] = buf;

	alloc_stack_next++;

	return;
}

void *
acl_alloc(unsigned long len)
{
	void *ret;

	if (len > PAGE_SIZE)
		BUG();

	ret = kmalloc(len, GFP_KERNEL);

	if (ret)
		alloc_push(ret);

	return ret;
}

void
acl_free_all(void)
{
	if (gr_acl_is_enabled() || !alloc_stack)
		return;

	while (alloc_pop()) ;

	if (alloc_stack) {
		if ((alloc_stack_size * sizeof (void *)) <= PAGE_SIZE)
			kfree(alloc_stack);
		else
			vfree(alloc_stack);
	}

	alloc_stack = NULL;
	alloc_stack_size = 1;
	alloc_stack_next = 1;

	return;
}

int
acl_alloc_stack_init(unsigned long size)
{
	if ((size * sizeof (void *)) <= PAGE_SIZE)
		alloc_stack =
		    (void **) kmalloc(size * sizeof (void *), GFP_KERNEL);
	else
		alloc_stack = (void **) vmalloc(size * sizeof (void *));

	alloc_stack_size = size;

	if (!alloc_stack)
		return 0;
	else
		return 1;
}
