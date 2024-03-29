#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/ipc.h>
#include <linux/gracl.h>
#include <linux/grsecurity.h>
#include <linux/grinternal.h>

int
gr_handle_shmat(const pid_t shm_cprid, const pid_t shm_lapid,
		const time_t shm_createtime, const uid_t cuid, const int shmid)
{
	struct task_struct *task;

	if (!gr_acl_is_enabled())
		return 1;

	read_lock(&tasklist_lock);

	task = find_task_by_vpid(shm_cprid);

	if (unlikely(!task))
		task = find_task_by_vpid(shm_lapid);

	if (unlikely(task && (time_before_eq((unsigned long)task->start_time.tv_sec, (unsigned long)shm_createtime) ||
			      (task->pid == shm_lapid)) &&
		     (task->acl->mode & GR_PROTSHM) &&
		     (task->acl != current->acl))) {
		read_unlock(&tasklist_lock);
		gr_log_int3(GR_DONT_AUDIT, GR_SHMAT_ACL_MSG, cuid, shm_cprid, shmid);
		return 0;
	}
	read_unlock(&tasklist_lock);

	return 1;
}
