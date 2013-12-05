// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef XIO_CHECK_H
#define XIO_CHECK_H

#define CHECK_LOCK

struct check_aio_aspect {
	GENERIC_ASPECT(aio);
#ifdef CHECK_LOCK
	struct list_head aio_head;
#endif
	struct generic_callback cb;
	struct check_output *output;
	unsigned long last_jiffies;
	atomic_t call_count;
	atomic_t callback_count;
	bool installed;
};

struct check_brick {
	XIO_BRICK(check);
};

struct check_input {
	XIO_INPUT(check);
};

struct check_output {
	XIO_OUTPUT(check);
	int instance_nr;
#ifdef CHECK_LOCK
	struct task_struct *watchdog;
	spinlock_t check_lock;
	struct list_head aio_anchor;
#endif
};

XIO_TYPES(check);

#endif
