// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef XIO_SERVER_H
#define XIO_SERVER_H

#include <linux/wait.h>

#include "xio_net.h"
#include "../lib_limiter.h"

extern int server_show_statist;

extern struct xio_limiter server_limiter;

struct server_aio_aspect {
	GENERIC_ASPECT(aio);
	struct server_brick *brick;
	struct list_head cb_head;
};

struct server_output {
	XIO_OUTPUT(server);
};

struct server_brick {
	XIO_BRICK(server);
	atomic_t in_flight;
	struct semaphore socket_sem;
	struct xio_socket handler_socket;
	struct task_struct *handler_thread;
	struct task_struct *cb_thread;
	wait_queue_head_t startup_event;
	wait_queue_head_t cb_event;
	spinlock_t cb_lock;
	struct list_head cb_read_list;
	struct list_head cb_write_list;
	bool cb_running;
	bool handler_running;
};

struct server_input {
	XIO_INPUT(server);
};

XIO_TYPES(server);

#endif
