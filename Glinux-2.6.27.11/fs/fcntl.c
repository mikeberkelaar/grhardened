/*
 *  linux/fs/fcntl.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/syscalls.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/capability.h>
#include <linux/dnotify.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/security.h>
#include <linux/ptrace.h>
#include <linux/signal.h>
#include <linux/rcupdate.h>
#include <linux/pid_namespace.h>
#include <linux/grsecurity.h>
#include <linux/smp_lock.h>

#include <asm/poll.h>
#include <asm/siginfo.h>
#include <asm/uaccess.h>

void set_close_on_exec(unsigned int fd, int flag)
{
	struct files_struct *files = current->files;
	struct fdtable *fdt;
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (flag)
		FD_SET(fd, fdt->close_on_exec);
	else
		FD_CLR(fd, fdt->close_on_exec);
	spin_unlock(&files->file_lock);
}

static int get_close_on_exec(unsigned int fd)
{
	struct files_struct *files = current->files;
	struct fdtable *fdt;
	int res;
	rcu_read_lock();
	fdt = files_fdtable(files);
	res = FD_ISSET(fd, fdt->close_on_exec);
	rcu_read_unlock();
	return res;
}

asmlinkage long sys_dup3(unsigned int oldfd, unsigned int newfd, int flags)
{
	int err = -EBADF;
	struct file * file, *tofree;
	struct files_struct * files = current->files;
	struct fdtable *fdt;

	if ((flags & ~O_CLOEXEC) != 0)
		return -EINVAL;

	if (unlikely(oldfd == newfd))
		return -EINVAL;

	spin_lock(&files->file_lock);
	err = expand_files(files, newfd);
	file = fcheck(oldfd);
	if (unlikely(!file))
		goto Ebadf;
	if (unlikely(err < 0)) {
		if (err == -EMFILE)
			goto Ebadf;
		goto out_unlock;
	}
	/*
	 * We need to detect attempts to do dup2() over allocated but still
	 * not finished descriptor.  NB: OpenBSD avoids that at the price of
	 * extra work in their equivalent of fget() - they insert struct
	 * file immediately after grabbing descriptor, mark it larval if
	 * more work (e.g. actual opening) is needed and make sure that
	 * fget() treats larval files as absent.  Potentially interesting,
	 * but while extra work in fget() is trivial, locking implications
	 * and amount of surgery on open()-related paths in VFS are not.
	 * FreeBSD fails with -EBADF in the same situation, NetBSD "solution"
	 * deadlocks in rather amusing ways, AFAICS.  All of that is out of
	 * scope of POSIX or SUS, since neither considers shared descriptor
	 * tables and this condition does not arise without those.
	 */
	err = -EBUSY;
	fdt = files_fdtable(files);
	tofree = fdt->fd[newfd];
	if (!tofree && FD_ISSET(newfd, fdt->open_fds))
		goto out_unlock;
	get_file(file);
	rcu_assign_pointer(fdt->fd[newfd], file);
	FD_SET(newfd, fdt->open_fds);
	if (flags & O_CLOEXEC)
		FD_SET(newfd, fdt->close_on_exec);
	else
		FD_CLR(newfd, fdt->close_on_exec);
	spin_unlock(&files->file_lock);

	if (tofree)
		filp_close(tofree, files);

	return newfd;

Ebadf:
	err = -EBADF;
out_unlock:
	spin_unlock(&files->file_lock);
	return err;
}

asmlinkage long sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	if (unlikely(newfd == oldfd)) { /* corner case */
		struct files_struct *files = current->files;
		rcu_read_lock();
		if (!fcheck_files(files, oldfd))
			oldfd = -EBADF;
		rcu_read_unlock();
		return oldfd;
	}
	return sys_dup3(oldfd, newfd, 0);
}

asmlinkage long sys_dup(unsigned int fildes)
{
	int ret = -EBADF;
	struct file *file = fget(fildes);

	if (file) {
		ret = get_unused_fd();
		if (ret >= 0)
			fd_install(ret, file);
		else
			fput(file);
	}
	return ret;
}

#define SETFL_MASK (O_APPEND | O_NONBLOCK | O_NDELAY | FASYNC | O_DIRECT | O_NOATIME)

static int setfl(int fd, struct file * filp, unsigned long arg)
{
	struct inode * inode = filp->f_path.dentry->d_inode;
	int error = 0;

	/*
	 * O_APPEND cannot be cleared if the file is marked as append-only
	 * and the file is open for write.
	 */
	if (((arg ^ filp->f_flags) & O_APPEND) && IS_APPEND(inode))
		return -EPERM;

	/* O_NOATIME can only be set by the owner or superuser */
	if ((arg & O_NOATIME) && !(filp->f_flags & O_NOATIME))
		if (!is_owner_or_cap(inode))
			return -EPERM;

	/* required for strict SunOS emulation */
	if (O_NONBLOCK != O_NDELAY)
	       if (arg & O_NDELAY)
		   arg |= O_NONBLOCK;

	if (arg & O_DIRECT) {
		if (!filp->f_mapping || !filp->f_mapping->a_ops ||
			!filp->f_mapping->a_ops->direct_IO)
				return -EINVAL;
	}

	if (filp->f_op && filp->f_op->check_flags)
		error = filp->f_op->check_flags(arg);
	if (error)
		return error;

	/*
	 * We still need a lock here for now to keep multiple FASYNC calls
	 * from racing with each other.
	 */
	lock_kernel();
	if ((arg ^ filp->f_flags) & FASYNC) {
		if (filp->f_op && filp->f_op->fasync) {
			error = filp->f_op->fasync(fd, filp, (arg & FASYNC) != 0);
			if (error < 0)
				goto out;
		}
	}

	filp->f_flags = (arg & SETFL_MASK) | (filp->f_flags & ~SETFL_MASK);
 out:
	unlock_kernel();
	return error;
}

static void f_modown(struct file *filp, struct pid *pid, enum pid_type type,
                     uid_t uid, uid_t euid, int force)
{
	write_lock_irq(&filp->f_owner.lock);
	if (force || !filp->f_owner.pid) {
		put_pid(filp->f_owner.pid);
		filp->f_owner.pid = get_pid(pid);
		filp->f_owner.pid_type = type;
		filp->f_owner.uid = uid;
		filp->f_owner.euid = euid;
	}
	write_unlock_irq(&filp->f_owner.lock);
}

int __f_setown(struct file *filp, struct pid *pid, enum pid_type type,
		int force)
{
	int err;
	
	err = security_file_set_fowner(filp);
	if (err)
		return err;

	f_modown(filp, pid, type, current->uid, current->euid, force);
	return 0;
}
EXPORT_SYMBOL(__f_setown);

int f_setown(struct file *filp, unsigned long arg, int force)
{
	enum pid_type type;
	struct pid *pid;
	int who = arg;
	int result;
	type = PIDTYPE_PID;
	if (who < 0) {
		type = PIDTYPE_PGID;
		who = -who;
	}
	rcu_read_lock();
	pid = find_vpid(who);
	result = __f_setown(filp, pid, type, force);
	rcu_read_unlock();
	return result;
}
EXPORT_SYMBOL(f_setown);

void f_delown(struct file *filp)
{
	f_modown(filp, NULL, PIDTYPE_PID, 0, 0, 1);
}

pid_t f_getown(struct file *filp)
{
	pid_t pid;
	read_lock(&filp->f_owner.lock);
	pid = pid_vnr(filp->f_owner.pid);
	if (filp->f_owner.pid_type == PIDTYPE_PGID)
		pid = -pid;
	read_unlock(&filp->f_owner.lock);
	return pid;
}

static long do_fcntl(int fd, unsigned int cmd, unsigned long arg,
		struct file *filp)
{
	long err = -EINVAL;

	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		gr_learn_resource(current, RLIMIT_NOFILE, arg, 0);
		if (arg >= current->signal->rlim[RLIMIT_NOFILE].rlim_cur)
			break;
		err = alloc_fd(arg, cmd == F_DUPFD_CLOEXEC ? O_CLOEXEC : 0);
		if (err >= 0) {
			get_file(filp);
			fd_install(err, filp);
		}
		break;
	case F_GETFD:
		err = get_close_on_exec(fd) ? FD_CLOEXEC : 0;
		break;
	case F_SETFD:
		err = 0;
		set_close_on_exec(fd, arg & FD_CLOEXEC);
		break;
	case F_GETFL:
		err = filp->f_flags;
		break;
	case F_SETFL:
		err = setfl(fd, filp, arg);
		break;
	case F_GETLK:
		err = fcntl_getlk(filp, (struct flock __user *) arg);
		break;
	case F_SETLK:
	case F_SETLKW:
		err = fcntl_setlk(fd, filp, cmd, (struct flock __user *) arg);
		break;
	case F_GETOWN:
		/*
		 * XXX If f_owner is a process group, the
		 * negative return value will get converted
		 * into an error.  Oops.  If we keep the
		 * current syscall conventions, the only way
		 * to fix this will be in libc.
		 */
		err = f_getown(filp);
		force_successful_syscall_return();
		break;
	case F_SETOWN:
		err = f_setown(filp, arg, 1);
		break;
	case F_GETSIG:
		err = filp->f_owner.signum;
		break;
	case F_SETSIG:
		/* arg == 0 restores default behaviour. */
		if (!valid_signal(arg)) {
			break;
		}
		err = 0;
		filp->f_owner.signum = arg;
		break;
	case F_GETLEASE:
		err = fcntl_getlease(filp);
		break;
	case F_SETLEASE:
		err = fcntl_setlease(fd, filp, arg);
		break;
	case F_NOTIFY:
		err = fcntl_dirnotify(fd, filp, arg);
		break;
	default:
		break;
	}
	return err;
}

asmlinkage long sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file *filp;
	long err = -EBADF;

	filp = fget(fd);
	if (!filp)
		goto out;

	err = security_file_fcntl(filp, cmd, arg);
	if (err) {
		fput(filp);
		return err;
	}

	err = do_fcntl(fd, cmd, arg, filp);

 	fput(filp);
out:
	return err;
}

#if BITS_PER_LONG == 32
asmlinkage long sys_fcntl64(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;
	long err;

	err = -EBADF;
	filp = fget(fd);
	if (!filp)
		goto out;

	err = security_file_fcntl(filp, cmd, arg);
	if (err) {
		fput(filp);
		return err;
	}
	err = -EBADF;
	
	switch (cmd) {
		case F_GETLK64:
			err = fcntl_getlk64(filp, (struct flock64 __user *) arg);
			break;
		case F_SETLK64:
		case F_SETLKW64:
			err = fcntl_setlk64(fd, filp, cmd,
					(struct flock64 __user *) arg);
			break;
		default:
			err = do_fcntl(fd, cmd, arg, filp);
			break;
	}
	fput(filp);
out:
	return err;
}
#endif

/* Table to convert sigio signal codes into poll band bitmaps */

static const long band_table[NSIGPOLL] = {
	POLLIN | POLLRDNORM,			/* POLL_IN */
	POLLOUT | POLLWRNORM | POLLWRBAND,	/* POLL_OUT */
	POLLIN | POLLRDNORM | POLLMSG,		/* POLL_MSG */
	POLLERR,				/* POLL_ERR */
	POLLPRI | POLLRDBAND,			/* POLL_PRI */
	POLLHUP | POLLERR			/* POLL_HUP */
};

static inline int sigio_perm(struct task_struct *p,
                             struct fown_struct *fown, int sig)
{
	return (((fown->euid == 0) ||
		 (fown->euid == p->suid) || (fown->euid == p->uid) ||
		 (fown->uid == p->suid) || (fown->uid == p->uid)) &&
		!security_file_send_sigiotask(p, fown, sig) &&
		!gr_check_protected_task(p) && !gr_pid_is_chrooted(p));
}

static void send_sigio_to_task(struct task_struct *p,
			       struct fown_struct *fown, 
			       int fd,
			       int reason)
{
	if (!sigio_perm(p, fown, fown->signum))
		return;

	switch (fown->signum) {
		siginfo_t si;
		default:
			/* Queue a rt signal with the appropriate fd as its
			   value.  We use SI_SIGIO as the source, not 
			   SI_KERNEL, since kernel signals always get 
			   delivered even if we can't queue.  Failure to
			   queue in this case _should_ be reported; we fall
			   back to SIGIO in that case. --sct */
			si.si_signo = fown->signum;
			si.si_errno = 0;
		        si.si_code  = reason;
			/* Make sure we are called with one of the POLL_*
			   reasons, otherwise we could leak kernel stack into
			   userspace.  */
			BUG_ON((reason & __SI_MASK) != __SI_POLL);
			if (reason - POLL_IN >= NSIGPOLL)
				si.si_band  = ~0L;
			else
				si.si_band = band_table[reason - POLL_IN];
			si.si_fd    = fd;
			if (!group_send_sig_info(fown->signum, &si, p))
				break;
		/* fall-through: fall back on the old plain SIGIO signal */
		case 0:
			group_send_sig_info(SIGIO, SEND_SIG_PRIV, p);
	}
}

void send_sigio(struct fown_struct *fown, int fd, int band)
{
	struct task_struct *p;
	enum pid_type type;
	struct pid *pid;
	
	read_lock(&fown->lock);
	type = fown->pid_type;
	pid = fown->pid;
	if (!pid)
		goto out_unlock_fown;
	
	read_lock(&tasklist_lock);
	do_each_pid_task(pid, type, p) {
		send_sigio_to_task(p, fown, fd, band);
	} while_each_pid_task(pid, type, p);
	read_unlock(&tasklist_lock);
 out_unlock_fown:
	read_unlock(&fown->lock);
}

static void send_sigurg_to_task(struct task_struct *p,
                                struct fown_struct *fown)
{
	if (sigio_perm(p, fown, SIGURG))
		group_send_sig_info(SIGURG, SEND_SIG_PRIV, p);
}

int send_sigurg(struct fown_struct *fown)
{
	struct task_struct *p;
	enum pid_type type;
	struct pid *pid;
	int ret = 0;
	
	read_lock(&fown->lock);
	type = fown->pid_type;
	pid = fown->pid;
	if (!pid)
		goto out_unlock_fown;

	ret = 1;
	
	read_lock(&tasklist_lock);
	do_each_pid_task(pid, type, p) {
		send_sigurg_to_task(p, fown);
	} while_each_pid_task(pid, type, p);
	read_unlock(&tasklist_lock);
 out_unlock_fown:
	read_unlock(&fown->lock);
	return ret;
}

static DEFINE_RWLOCK(fasync_lock);
static struct kmem_cache *fasync_cache __read_mostly;

/*
 * fasync_helper() is used by some character device drivers (mainly mice)
 * to set up the fasync queue. It returns negative on error, 0 if it did
 * no changes and positive if it added/deleted the entry.
 */
int fasync_helper(int fd, struct file * filp, int on, struct fasync_struct **fapp)
{
	struct fasync_struct *fa, **fp;
	struct fasync_struct *new = NULL;
	int result = 0;

	if (on) {
		new = kmem_cache_alloc(fasync_cache, GFP_KERNEL);
		if (!new)
			return -ENOMEM;
	}
	write_lock_irq(&fasync_lock);
	for (fp = fapp; (fa = *fp) != NULL; fp = &fa->fa_next) {
		if (fa->fa_file == filp) {
			if(on) {
				fa->fa_fd = fd;
				kmem_cache_free(fasync_cache, new);
			} else {
				*fp = fa->fa_next;
				kmem_cache_free(fasync_cache, fa);
				result = 1;
			}
			goto out;
		}
	}

	if (on) {
		new->magic = FASYNC_MAGIC;
		new->fa_file = filp;
		new->fa_fd = fd;
		new->fa_next = *fapp;
		*fapp = new;
		result = 1;
	}
out:
	write_unlock_irq(&fasync_lock);
	return result;
}

EXPORT_SYMBOL(fasync_helper);

void __kill_fasync(struct fasync_struct *fa, int sig, int band)
{
	while (fa) {
		struct fown_struct * fown;
		if (fa->magic != FASYNC_MAGIC) {
			printk(KERN_ERR "kill_fasync: bad magic number in "
			       "fasync_struct!\n");
			return;
		}
		fown = &fa->fa_file->f_owner;
		/* Don't send SIGURG to processes which have not set a
		   queued signum: SIGURG has its own default signalling
		   mechanism. */
		if (!(sig == SIGURG && fown->signum == 0))
			send_sigio(fown, fa->fa_fd, band);
		fa = fa->fa_next;
	}
}

EXPORT_SYMBOL(__kill_fasync);

void kill_fasync(struct fasync_struct **fp, int sig, int band)
{
	/* First a quick test without locking: usually
	 * the list is empty.
	 */
	if (*fp) {
		read_lock(&fasync_lock);
		/* reread *fp after obtaining the lock */
		__kill_fasync(*fp, sig, band);
		read_unlock(&fasync_lock);
	}
}
EXPORT_SYMBOL(kill_fasync);

static int __init fasync_init(void)
{
	fasync_cache = kmem_cache_create("fasync_cache",
		sizeof(struct fasync_struct), 0, SLAB_PANIC, NULL);
	return 0;
}

module_init(fasync_init)
