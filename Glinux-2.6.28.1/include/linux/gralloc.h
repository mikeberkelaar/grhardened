#ifndef __GRALLOC_H
#define __GRALLOC_H

void acl_free_all(void);
int acl_alloc_stack_init(unsigned long size);
void *acl_alloc(unsigned long len);

#endif
