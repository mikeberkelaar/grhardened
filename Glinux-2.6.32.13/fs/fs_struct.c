#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/fs_struct.h>

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block.
 */
void set_fs_root(struct fs_struct *fs, struct path *path)
{
	struct path old_root;

	write_lock(&fs->lock);
	old_root = fs->root;
	fs->root = *path;
	path_get(path);
	write_unlock(&fs->lock);
	if (old_root.dentry)
		path_put(&old_root);
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block.
 */
void set_fs_pwd(struct fs_struct *fs, struct path *path)
{
	struct path old_pwd;

	write_lock(&fs->lock);
	old_pwd = fs->pwd;
	fs->pwd = *path;
	path_get(path);
	write_unlock(&fs->lock);

	if (old_pwd.dentry)
		path_put(&old_pwd);
}

void chroot_fs_refs(struct path *old_root, struct path *new_root)
{
	struct task_struct *g, *p;
	struct fs_struct *fs;
	int count = 0;
	unsigned long flags;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_lock(p);
		gr_fs_write_lock_irqsave(p, flags);
		fs = p->fs;
		if (fs) {
			write_lock(&fs->lock);
			if (fs->root.dentry == old_root->dentry
			    && fs->root.mnt == old_root->mnt) {
				path_get(new_root);
				fs->root = *new_root;
				count++;
			}
			if (fs->pwd.dentry == old_root->dentry
			    && fs->pwd.mnt == old_root->mnt) {
				path_get(new_root);
				fs->pwd = *new_root;
				count++;
			}
			write_unlock(&fs->lock);
		}
		gr_fs_write_unlock_irqrestore(p, flags);
		task_unlock(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
	while (count--)
		path_put(old_root);
}

void free_fs_struct(struct fs_struct *fs)
{
	path_put(&fs->root);
	path_put(&fs->pwd);
	kmem_cache_free(fs_cachep, fs);
}

void exit_fs(struct task_struct *tsk)
{
	struct fs_struct *fs = tsk->fs;
	unsigned long flags;

	if (fs) {
		int kill;
		task_lock(tsk);
		gr_fs_write_lock_irqsave(tsk, flags);
		write_lock(&fs->lock);
		tsk->fs = NULL;
		kill = !atomic_dec_return(&fs->users);
		write_unlock(&fs->lock);
		gr_fs_write_unlock_irqrestore(tsk, flags);
		task_unlock(tsk);
		if (kill)
			free_fs_struct(fs);
	}
}

struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
	struct fs_struct *fs = kmem_cache_alloc(fs_cachep, GFP_KERNEL);
	/* We don't need to lock fs - think why ;-) */
	if (fs) {
		atomic_set(&fs->users, 1);
		fs->in_exec = 0;
		rwlock_init(&fs->lock);
		fs->umask = old->umask;
		read_lock(&old->lock);
		fs->root = old->root;
		path_get(&old->root);
		fs->pwd = old->pwd;
		path_get(&old->pwd);
		read_unlock(&old->lock);
	}
	return fs;
}

int unshare_fs_struct(void)
{
	struct fs_struct *fs = current->fs;
	struct fs_struct *new_fs = copy_fs_struct(fs);
	int kill;
	unsigned long flags;

	if (!new_fs)
		return -ENOMEM;

	task_lock(current);
	gr_fs_write_lock_irqsave(current, flags);
	write_lock(&fs->lock);
	kill = !atomic_dec_return(&fs->users);
	current->fs = new_fs;
	write_unlock(&fs->lock);
	gr_fs_write_unlock_irqrestore(current, flags);
	task_unlock(current);

	if (kill)
		free_fs_struct(fs);

	return 0;
}
EXPORT_SYMBOL_GPL(unshare_fs_struct);

int current_umask(void)
{
	return current->fs->umask;
}
EXPORT_SYMBOL(current_umask);

/* to be mentioned only in INIT_TASK */
struct fs_struct init_fs = {
	.users		= ATOMIC_INIT(1),
	.lock		= __RW_LOCK_UNLOCKED(init_fs.lock),
	.umask		= 0022,
};

void daemonize_fs_struct(void)
{
	struct fs_struct *fs = current->fs;
	unsigned long flags;

	if (fs) {
		int kill;

		task_lock(current);

		write_lock(&init_fs.lock);
		atomic_inc(&init_fs.users);
		write_unlock(&init_fs.lock);

		gr_fs_write_lock_irqsave(current, flags);
		write_lock(&fs->lock);
		current->fs = &init_fs;
		kill = !atomic_dec_return(&fs->users);
		write_unlock(&fs->lock);
		gr_fs_write_unlock_irqrestore(current, flags);

		task_unlock(current);
		if (kill)
			free_fs_struct(fs);
	}
}
