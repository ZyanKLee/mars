// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

// Server brick (just for demonstration)

//#define BRICK_DEBUGGING
//#define XIO_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "../mars_light/light_strategy.h"

#include "xio.h"
#include "xio_bio.h"
#include "xio_aio.h"
#include "unused/xio_sio.h"

///////////////////////// own type definitions ////////////////////////

#include "xio_server.h"

#define NR_SOCKETS			3

static struct xio_socket server_socket[NR_SOCKETS] = {};
static struct task_struct *server_thread[NR_SOCKETS] = {};

///////////////////////// own helper functions ////////////////////////


static
int cb_thread(void *data)
{
	struct server_brick *brick = data;
	struct xio_socket *sock = &brick->handler_socket;
	bool aborted = false;
	bool ok = xio_get_socket(sock);
	int status = -EINVAL;

	XIO_DBG("--------------- cb_thread starting on socket #%d, ok = %d\n", sock->s_debug_nr, ok);
	if (!ok)
		goto done;

	brick->cb_running = true;
	wake_up_interruptible(&brick->startup_event);

	while (!brick_thread_should_stop() || !list_empty(&brick->cb_read_list) || !list_empty(&brick->cb_write_list) || atomic_read(&brick->in_flight) > 0) {
		struct server_aio_aspect *aio_a;
		struct aio_object *aio;
		struct list_head *tmp;

		wait_event_interruptible_timeout(
			brick->cb_event,
			!list_empty(&brick->cb_read_list) ||
			!list_empty(&brick->cb_write_list),
			1 * HZ);

		spin_lock(&brick->cb_lock);
		tmp = brick->cb_write_list.next;
		if (tmp == &brick->cb_write_list) {
			tmp = brick->cb_read_list.next;
			if (tmp == &brick->cb_read_list) {
				spin_unlock(&brick->cb_lock);
				brick_msleep(1000 / HZ);
				continue;
			}
		}
		list_del_init(tmp);
		spin_unlock(&brick->cb_lock);

		aio_a = container_of(tmp, struct server_aio_aspect, cb_head);
		aio = aio_a->object;
		status = -EINVAL;
		CHECK_PTR(aio, err);

		status = 0;
		if (!aborted) {
			down(&brick->socket_sem);
			status = xio_send_cb(sock, aio);
			up(&brick->socket_sem);
		}

	err:
		if (unlikely(status < 0) && !aborted) {
			aborted = true;
			XIO_WRN("cannot send response, status = %d\n", status);
			/* Just shutdown the socket and forget all pending
			 * requests.
			 * The _client_ is responsible for resending
			 * any lost operations.
			 */
			xio_shutdown_socket(sock);
		}

		GENERIC_INPUT_CALL(brick->inputs[0], aio_put, aio);
		atomic_dec(&brick->in_flight);
	}

	xio_shutdown_socket(sock);
	xio_put_socket(sock);

done:
	XIO_DBG("---------- cb_thread terminating, status = %d\n", status);
	wake_up_interruptible(&brick->startup_event);
	return status;
}

static
void server_endio(struct generic_callback *cb)
{
	struct server_aio_aspect *aio_a;
	struct aio_object *aio;
	struct server_brick *brick;
	int rw;

	aio_a = cb->cb_private;
	CHECK_PTR(aio_a, err);
	aio = aio_a->object;
	CHECK_PTR(aio, err);

	brick = aio_a->brick;
	if (!brick) {
		XIO_WRN("late IO callback -- cannot do anything\n");
		return;
	}

	rw = aio->io_rw;

	spin_lock(&brick->cb_lock);
	if (rw) {
		list_add_tail(&aio_a->cb_head, &brick->cb_write_list);
	} else {
		list_add_tail(&aio_a->cb_head, &brick->cb_read_list);
	}
	spin_unlock(&brick->cb_lock);

	wake_up_interruptible(&brick->cb_event);
	return;
err:
	XIO_FAT("cannot handle callback - giving up\n");
}

int server_io(struct server_brick *brick, struct xio_socket *sock, struct xio_cmd *cmd)
{
	struct aio_object *aio;
	struct server_aio_aspect *aio_a;
	int amount;
	int status = -ENOTRECOVERABLE;

	if (!brick->cb_running || !brick->handler_running || !xio_socket_is_alive(sock))
		goto done;

	aio = server_alloc_aio(brick);
	status = -ENOMEM;
	aio_a = server_aio_get_aspect(brick, aio);
	if (unlikely(!aio_a)) {
		obj_free(aio);
		goto done;
	}

	status = xio_recv_aio(sock, aio, cmd);
	if (status < 0) {
		obj_free(aio);
		goto done;
	}

	aio_a->brick = brick;
	SETUP_CALLBACK(aio, server_endio, aio_a);

	amount = 0;
	if (!aio->io_cs_mode < 2)
		amount = (aio->io_len - 1) / 1024 + 1;
	xio_limit_sleep(&server_limiter, amount);

	status = GENERIC_INPUT_CALL(brick->inputs[0], aio_get, aio);
	if (unlikely(status < 0)) {
		XIO_WRN("aio_get execution error = %d\n", status);
		SIMPLE_CALLBACK(aio, status);
		status = 0; // continue serving requests
		goto done;
	}

	atomic_inc(&brick->in_flight);
	GENERIC_INPUT_CALL(brick->inputs[0], aio_io, aio);

done:
	return status;
}

static
void _clean_list(struct server_brick *brick, struct list_head *start)
{
	for (;;) {
		struct server_aio_aspect *aio_a;
		struct aio_object *aio;
		struct list_head *tmp = start->next;
		if (tmp == start)
			break;

		list_del_init(tmp);

		aio_a = container_of(tmp, struct server_aio_aspect, cb_head);
		aio_a->brick = NULL;
		aio = aio_a->object;
		if (!aio)
			continue;

		GENERIC_INPUT_CALL(brick->inputs[0], aio_put, aio);
	}
}

static
int _set_server_sio_params(struct xio_brick *_brick, void *private)
{
	struct sio_brick *sio_brick = (void*)_brick;
	if (_brick->type != (void*)_sio_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	sio_brick->o_direct = false;
	sio_brick->o_fdsync = false;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int _set_server_aio_params(struct xio_brick *_brick, void *private)
{
	struct aio_brick *aio_brick = (void*)_brick;
	if (_brick->type == (void*)_sio_brick_type) {
		return _set_server_sio_params(_brick, private);
	}
	if (_brick->type != (void*)_aio_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	aio_brick->o_creat = false;
	aio_brick->o_direct = false;
	aio_brick->o_fdsync = false;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int _set_server_bio_params(struct xio_brick *_brick, void *private)
{
	struct bio_brick *bio_brick;
	if (_brick->type == (void*)_aio_brick_type) {
		return _set_server_aio_params(_brick, private);
	}
	if (_brick->type == (void*)_sio_brick_type) {
		return _set_server_sio_params(_brick, private);
	}
	if (_brick->type != (void*)_bio_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	bio_brick = (void*)_brick;
	bio_brick->ra_pages = 0;
	bio_brick->do_noidle = true;
	bio_brick->do_sync = true;
	bio_brick->do_unplug = true;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int dummy_worker(struct xio_global *global, struct xio_dent *dent, bool prepare, bool direction)
{
	return 0;
}

static
int handler_thread(void *data)
{
	struct server_brick *brick = data;
	struct xio_socket *sock = &brick->handler_socket;
	bool ok = xio_get_socket(sock);
	int debug_nr;
	int status = -EINVAL;

	XIO_DBG("#%d --------------- handler_thread starting on socket %p\n", sock->s_debug_nr, sock);
	if (!ok)
		goto done;

	brick->handler_running = true;
	wake_up_interruptible(&brick->startup_event);

	while (!brick_thread_should_stop() && xio_socket_is_alive(sock)) {
		struct xio_cmd cmd = {};

		status = -EINTR;
		if (unlikely(!xio_global || !xio_global->global_power.button)) {
			XIO_DBG("system is not alive\n");
			break;
		}

		status = xio_recv_struct(sock, &cmd, xio_cmd_meta);
		if (unlikely(status < 0)) {
			XIO_WRN("#%d recv cmd status = %d\n", sock->s_debug_nr, status);
			goto clean;
		}
		if (unlikely(!xio_socket_is_alive(sock))) {
			XIO_WRN("#%d is dead\n", sock->s_debug_nr);
			status = -EINTR;
			goto clean;
		}

		status = -EPROTO;
		switch (cmd.cmd_code & CMD_FLAG_MASK) {
		case CMD_NOP:
			status = 0;
			XIO_DBG("#%d got NOP operation\n", sock->s_debug_nr);
			break;
		case CMD_NOTIFY:
			status = 0;
			from_remote_trigger();
			break;
		case CMD_GETINFO:
		{
			struct xio_info info = {};
			status = GENERIC_INPUT_CALL(brick->inputs[0], xio_get_info, &info);
			if (status < 0) {
				break;
			}
			down(&brick->socket_sem);
			status = xio_send_struct(sock, &cmd, xio_cmd_meta);
			if (status >= 0) {
				status = xio_send_struct(sock, &info, xio_info_meta);
			}
			up(&brick->socket_sem);
			break;
		}
		case CMD_GETENTS:
		{
			struct xio_global local = {
				.dent_anchor = LIST_HEAD_INIT(local.dent_anchor),
				.brick_anchor = LIST_HEAD_INIT(local.brick_anchor),
				.global_power = {
					.button = true,
				},
				.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(local.main_event),
			};

			status = -EINVAL;
			if (unlikely(!cmd.cmd_str1))
				break;

			init_rwsem(&local.dent_mutex);
			init_rwsem(&local.brick_mutex);

			status = xio_dent_work(&local, "/mars", sizeof(struct xio_dent), external_checker, dummy_worker, &local, 3);

			down(&brick->socket_sem);
			status = xio_send_dent_list(sock, &local.dent_anchor);
			up(&brick->socket_sem);

			if (status < 0) {
				XIO_WRN("#%d could not send dentry information, status = %d\n", sock->s_debug_nr, status);
			}

			xio_free_dent_all(&local, &local.dent_anchor);
			break;
		}
		case CMD_CONNECT:
		{
			struct xio_brick *prev;
			const char *path = cmd.cmd_str1;

			status = -EINVAL;
			CHECK_PTR(path, err);
			CHECK_PTR_NULL(_bio_brick_type, err);

			if (!brick->global || !xio_global || !xio_global->global_power.button) {
				XIO_WRN("#%d system is not alive\n", sock->s_debug_nr);
				goto err;
			}

			prev = make_brick_all(
				brick->global,
				NULL,
				_set_server_bio_params,
				NULL,
				path,
				(const struct generic_brick_type*)_bio_brick_type,
				(const struct generic_brick_type*[]){},
				2, // start always
				path,
				(const char *[]){},
				0);
			if (likely(prev)) {
				status = generic_connect((void*)brick->inputs[0], (void*)prev->outputs[0]);
				if (unlikely(status < 0)) {
					XIO_ERR("#%d cannot connect to '%s'\n", sock->s_debug_nr, path);
				}
				prev->killme = true;
			} else {
				XIO_ERR("#%d cannot find brick '%s'\n", sock->s_debug_nr, path);
			}

		err:
			cmd.cmd_int1 = status;
			down(&brick->socket_sem);
			status = xio_send_struct(sock, &cmd, xio_cmd_meta);
			up(&brick->socket_sem);
			break;
		}
		case CMD_AIO:
		{
#ifdef CONFIG_MARS_LOADAVG_LIMIT // quirk
			int my_load = (avenrun[0] + FIXED_1/200) >> FSHIFT;
			if (xio_max_loadavg && my_load >= xio_max_loadavg) {
				XIO_WRN("#%d loadavg %d too high (%d), aborting data traffic\n", sock->s_debug_nr, my_load, xio_max_loadavg);
				status = -EBUSY;
				break;
			}
#endif
			status = server_io(brick, sock, &cmd);
			break;
		}
		case CMD_CB:
			XIO_ERR("#%d oops, as a server I should never get CMD_CB; something is wrong here - attack attempt??\n", sock->s_debug_nr);
			break;
		default:
			XIO_ERR("#%d unknown command %d\n", sock->s_debug_nr, cmd.cmd_code);
		}
	clean:
		brick_string_free(cmd.cmd_str1);
		if (status < 0)
			break;
	}

	xio_shutdown_socket(sock);
	xio_put_socket(sock);

 done:
	XIO_DBG("#%d handler_thread terminating, status = %d\n", sock->s_debug_nr, status);

	debug_nr = sock->s_debug_nr;

	XIO_DBG("#%d done.\n", debug_nr);
	brick->killme = true;
	return status;
}

////////////////// own brick / input / output operations //////////////////

static int server_get_info(struct server_output *output, struct xio_info *info)
{
	struct server_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, xio_get_info, info);
}

static int server_io_get(struct server_output *output, struct aio_object *aio)
{
	struct server_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, aio_get, aio);
}

static void server_io_put(struct server_output *output, struct aio_object *aio)
{
	struct server_input *input = output->brick->inputs[0];
	GENERIC_INPUT_CALL(input, aio_put, aio);
}

static void server_io_io(struct server_output *output, struct aio_object *aio)
{
	struct server_input *input = output->brick->inputs[0];
	GENERIC_INPUT_CALL(input, aio_io, aio);
}

static int server_switch(struct server_brick *brick)
{
	struct xio_socket *sock = &brick->handler_socket;
	int status = 0;

	if (brick->power.button) {
		static int version = 0;
		bool ok;

		if (brick->power.led_on)
			goto done;

		ok = xio_get_socket(sock);
		if (unlikely(!ok)) {
			status = -ENOENT;
			goto err;
		}

		xio_set_power_led_off((void*)brick, false);

		brick->cb_thread = brick_thread_create(cb_thread, brick, "xio_cb%d", version);
		if (unlikely(!brick->cb_thread)) {
			XIO_ERR("cannot create cb thread\n");
			status = -ENOENT;
			goto err;
		}

		brick->handler_thread = brick_thread_create(handler_thread, brick, "xio_handler%d", version++);
		if (unlikely(!brick->handler_thread)) {
			XIO_ERR("cannot create handler thread\n");
			brick_thread_stop(brick->cb_thread);
			brick->cb_thread = NULL;
			status = -ENOENT;
			goto err;
		}

		xio_set_power_led_on((void*)brick, true);
	} else if (!brick->power.led_off) {
		struct task_struct *thread;
		xio_set_power_led_on((void*)brick, false);

		xio_shutdown_socket(sock);

		thread = brick->handler_thread;
		if (thread) {
			brick->handler_thread = NULL;
			brick->handler_running = false;
			XIO_DBG("#%d stopping handler thread....\n", sock->s_debug_nr);
			brick_thread_stop(thread);
		}
		thread = brick->cb_thread;
		if (thread) {
			brick->cb_thread = NULL;
			brick->cb_running = false;
			XIO_DBG("#%d stopping callback thread....\n", sock->s_debug_nr);
			brick_thread_stop(thread);
		}

		xio_put_socket(sock);
		XIO_DBG("#%d socket s_count = %d\n", sock->s_debug_nr, atomic_read(&sock->s_count));

		// do this only after _both_ threads have stopped...
		_clean_list(brick, &brick->cb_read_list);
		_clean_list(brick, &brick->cb_write_list);

		xio_set_power_led_off((void*)brick, true);
	}
 err:
	if (unlikely(status < 0)) {
		xio_set_power_led_off((void*)brick, true);
		xio_shutdown_socket(sock);
		xio_put_socket(sock);
	}
done:
	return status;
}

//////////////// informational / statistics ///////////////

static
char *server_statistics(struct server_brick *brick, int verbose)
{
	char *res = brick_string_alloc(1024);

	snprintf(res, 1024,
		 "cb_running = %d "
		 "handler_running = %d "
		 "in_flight = %d\n",
		 brick->cb_running,
		 brick->handler_running,
		 atomic_read(&brick->in_flight));

	return res;
}

static
void server_reset_statistics(struct server_brick *brick)
{
}

//////////////// object / aspect constructors / destructors ///////////////

static int server_aio_aspect_init_fn(struct generic_aspect *_ini)
{
	struct server_aio_aspect *ini = (void*)_ini;
	INIT_LIST_HEAD(&ini->cb_head);
	return 0;
}

static void server_aio_aspect_exit_fn(struct generic_aspect *_ini)
{
	struct server_aio_aspect *ini = (void*)_ini;
	CHECK_HEAD_EMPTY(&ini->cb_head);
}

XIO_MAKE_STATICS(server);

////////////////////// brick constructors / destructors ////////////////////

static int server_brick_construct(struct server_brick *brick)
{
	init_waitqueue_head(&brick->startup_event);
	init_waitqueue_head(&brick->cb_event);
	sema_init(&brick->socket_sem, 1);
	spin_lock_init(&brick->cb_lock);
	INIT_LIST_HEAD(&brick->cb_read_list);
	INIT_LIST_HEAD(&brick->cb_write_list);
	return 0;
}

static int server_brick_destruct(struct server_brick *brick)
{
	CHECK_HEAD_EMPTY(&brick->cb_read_list);
	CHECK_HEAD_EMPTY(&brick->cb_write_list);
	return 0;
}

static int server_output_construct(struct server_output *output)
{
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct server_brick_ops server_brick_ops = {
	.brick_switch = server_switch,
	.brick_statistics = server_statistics,
	.reset_statistics = server_reset_statistics,
};

static struct server_output_ops server_output_ops = {
	.xio_get_info = server_get_info,
	.aio_get = server_io_get,
	.aio_put = server_io_put,
	.aio_io = server_io_io,
};

const struct server_input_type server_input_type = {
	.type_name = "server_input",
	.input_size = sizeof(struct server_input),
};

static const struct server_input_type *server_input_types[] = {
	&server_input_type,
};

const struct server_output_type server_output_type = {
	.type_name = "server_output",
	.output_size = sizeof(struct server_output),
	.master_ops = &server_output_ops,
	.output_construct = &server_output_construct,
};

static const struct server_output_type *server_output_types[] = {
	&server_output_type,
};

const struct server_brick_type server_brick_type = {
	.type_name = "server_brick",
	.brick_size = sizeof(struct server_brick),
	.max_inputs = 1,
	.max_outputs = 0,
	.master_ops = &server_brick_ops,
	.aspect_types = server_aspect_types,
	.default_input_types = server_input_types,
	.default_output_types = server_output_types,
	.brick_construct = &server_brick_construct,
	.brick_destruct = &server_brick_destruct,
};
EXPORT_SYMBOL_GPL(server_brick_type);

///////////////////////////////////////////////////////////////////////

// strategy layer

int server_show_statist = 0;
EXPORT_SYMBOL_GPL(server_show_statist);

static int _server_thread(void *data)
{
	struct xio_global server_global = {
		.dent_anchor = LIST_HEAD_INIT(server_global.dent_anchor),
		.brick_anchor = LIST_HEAD_INIT(server_global.brick_anchor),
		.global_power = {
			.button = true,
		},
		.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(server_global.main_event),
	};
	struct xio_socket *my_socket = data;
	char *id = my_id();
	int status = 0;

	init_rwsem(&server_global.dent_mutex);
	init_rwsem(&server_global.brick_mutex);

	XIO_INF("-------- server starting on host '%s' ----------\n", id);

	while (!brick_thread_should_stop() &&
	      (!xio_global || !xio_global->global_power.button)) {
		XIO_DBG("system did not start up\n");
		brick_msleep(5000);
	}

	XIO_INF("-------- server now working on host '%s' ----------\n", id);

	while (!brick_thread_should_stop() || !list_empty(&server_global.brick_anchor)) {
		struct server_brick *brick = NULL;
		struct xio_socket handler_socket = {};

		server_global.global_version++;

		if (server_show_statist)
			show_statistics(&server_global, "server");

		status = xio_kill_brick_when_possible(&server_global, &server_global.brick_anchor, false, NULL, true);
		XIO_DBG("kill server bricks (when possible) = %d\n", status);

		if (!xio_global || !xio_global->global_power.button) {
			brick_msleep(1000);
			continue;
		}

		status = xio_accept_socket(&handler_socket, my_socket);
		if (unlikely(status < 0 || !xio_socket_is_alive(&handler_socket))) {
			brick_msleep(500);
			if (status == -EAGAIN)
				continue; // without error message
			XIO_WRN("accept status = %d\n", status);
			brick_msleep(1000);
			continue;
		}
		handler_socket.s_shutdown_on_err = true;

		XIO_DBG("got new connection #%d\n", handler_socket.s_debug_nr);

		brick = (void*)xio_make_brick(&server_global, NULL, &server_brick_type, "handler", "handler");
		if (!brick) {
			XIO_ERR("cannot create server instance\n");
			xio_shutdown_socket(&handler_socket);
			xio_put_socket(&handler_socket);
			brick_msleep(2000);
			continue;
		}
		memcpy(&brick->handler_socket, &handler_socket, sizeof(struct xio_socket));

		/* TODO: check authorization.
		 */

		brick->power.button = true;
		status = server_switch(brick);
		if (unlikely(status < 0)) {
			XIO_ERR("cannot switch on server brick, status = %d\n", status);
			goto err;
		}

		// further references are usually held by the threads
		xio_put_socket(&brick->handler_socket);

		/* fire and forget....
		 * the new instance is now responsible for itself.
		 */
		brick = NULL;
		brick_msleep(100);
		continue;

	err:
		if (brick) {
			xio_shutdown_socket(&brick->handler_socket);
			xio_put_socket(&brick->handler_socket);
			status = xio_kill_brick((void*)brick);
			if (status < 0) {
				BRICK_ERR("kill status = %d, giving up\n", status);
			}
			brick = NULL;
		}
		brick_msleep(2000);
	}

	XIO_INF("-------- cleaning up ----------\n");

	xio_kill_brick_all(&server_global, &server_global.brick_anchor, false);

	//cleanup_mm();

	XIO_INF("-------- done status = %d ----------\n", status);
	return status;
}

////////////////// module init stuff /////////////////////////

struct xio_limiter server_limiter = {
	.lim_max_rate = 0,
};
EXPORT_SYMBOL_GPL(server_limiter);

void __exit exit_xio_server(void)
{
	int i;

	XIO_INF("exit_server()\n");
	server_unregister_brick_type();

	for (i = 0; i < NR_SOCKETS; i++) {
		if (server_thread[i]) {
			XIO_INF("stopping server thread %d...\n", i);
			brick_thread_stop(server_thread[i]);
		}
		XIO_INF("closing server socket %d...\n", i);
		xio_put_socket(&server_socket[i]);
	}
}

int __init init_xio_server(void)
{
	int i;

	XIO_INF("init_server()\n");

	for (i = 0; i < NR_SOCKETS; i++) {
		struct sockaddr_storage sockaddr = {};
		char tmp[16];
		int status;

		sprintf(tmp, ":%d", xio_net_default_port + i);
		status = xio_create_sockaddr(&sockaddr, tmp);
		if (unlikely(status < 0)) {
			exit_xio_server();
			return status;
		}

		status = xio_create_socket(&server_socket[i], &sockaddr, true);
		if (unlikely(status < 0)) {
			XIO_ERR("could not create server socket %d, status = %d\n", i, status);
			exit_xio_server();
			return status;
		}

		server_thread[i] = brick_thread_create(_server_thread, &server_socket[i], "xio_server_%d", i);
		if (unlikely(!server_thread[i] || IS_ERR(server_thread[i]))) {
			XIO_ERR("could not create server thread %d\n", i);
			exit_xio_server();
			return -ENOENT;
		}
	}

	return server_register_brick_type();
}
