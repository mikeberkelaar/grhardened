# grsecurity's ACL system was originally written in 2001 by Michael Dalton
# during 2001-2005 it has been completely redesigned by Brad Spengler
# into an RBAC system
#
# All code in this directory and various hooks inserted throughout the kernel
# are copyright Brad Spengler, and released under the GPL v2 or higher

obj-y = grsec_chdir.o grsec_chroot.o grsec_exec.o grsec_fifo.o grsec_fork.o \
	grsec_mount.o grsec_sig.o grsec_sock.o grsec_sysctl.o \
	grsec_time.o grsec_tpe.o grsec_ipc.o grsec_link.o grsec_textrel.o

obj-$(CONFIG_GRKERNSEC) += grsec_init.o grsum.o gracl.o gracl_ip.o gracl_segv.o \
	gracl_cap.o gracl_alloc.o gracl_shm.o grsec_mem.o gracl_fs.o \
	gracl_learn.o grsec_log.o
obj-$(CONFIG_GRKERNSEC_RESLOG) += gracl_res.o

ifndef CONFIG_GRKERNSEC
obj-y += grsec_disabled.o
endif

