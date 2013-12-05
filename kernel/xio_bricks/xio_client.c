// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

// Client brick (just for demonstration)

//#define BRICK_DEBUGGING
//#define XIO_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "xio.h"

///////////////////////// own type definitions ////////////////////////

#include "xio_client.h"

#define CLIENT_HASH_MAX 		(PAGE_SIZE / sizeof(struct list_head))

///////////////////////// own helper functions ////////////////////////

static int thread_count = 0;

static void _kill_thread(struct client_threadinfo *ti, const char *name)
{
	if (ti->thread) {
		XIO_DBG("stopping %s thread\n", name);
		brick_thread_stop(ti->thread);
		ti->thread = NULL;
	}
}

static void _kill_socket(struct client_output *output)
{
	output->brick->connection_state = 1;
	if (xio_socket_is_alive(&output->socket)) {
		XIO_DBG("shutdown socket\n");
		xio_shutdown_socket(&output->socket);
	}
	_kill_thread(&output->receiver, "receiver");
	output->recv_error = 0;
	XIO_DBG("close socket\n");
	xio_put_socket(&output->socket);
}

static int _request_info(struct client_output *output)
{
	struct xio_cmd cmd = {
		.cmd_code = CMD_GETINFO,
	};
	int status;

	XIO_DBG("\n");
	status = xio_send_struct(&output->socket, &cmd, xio_cmd_meta);
	if (unlikely(status < 0)) {
		XIO_DBG("send of getinfo failed, status = %d\n", status);
	}
	return status;
}

static int receiver_thread(void *data);

static int _connect(struct client_output *output, const char *str)
{
	struct sockaddr_storage sockaddr = {};
	int status;

	if (unlikely(!output->path)) {
		output->path = brick_strdup(str);
		status = -EINVAL;
		output->host = strchr(output->path, '@');
		if (!output->host) {
			brick_string_free(output->path);
			output->path = NULL;
			XIO_ERR("parameter string '%s' contains no remote specifier with '@'-syntax\n", str);
			goto done;
		}
		*output->host++ = '\0';
	}

	if (unlikely(output->receiver.thread)) {
		XIO_WRN("receiver thread unexpectedly not dead\n");
		_kill_thread(&output->receiver, "receiver");
	}

	status = xio_create_sockaddr(&sockaddr, output->host);
	if (unlikely(status < 0)) {
		XIO_DBG("no sockaddr, status = %d\n", status);
		goto done;
	}

	status = xio_create_socket(&output->socket, &sockaddr, false);
	if (unlikely(status < 0)) {
		XIO_DBG("no socket, status = %d\n", status);
		goto really_done;
	}
	output->socket.s_shutdown_on_err = true;

	output->receiver.thread = brick_thread_create(receiver_thread, output, "xio_receiver%d", thread_count++);
	if (unlikely(!output->receiver.thread)) {
		XIO_ERR("cannot start receiver thread, status = %d\n", status);
		status = -ENOENT;
		goto done;
	}


	{
		struct xio_cmd cmd = {
			.cmd_code = CMD_CONNECT,
			.cmd_str1 = output->path,
		};

		status = xio_send_struct(&output->socket, &cmd, xio_cmd_meta);
		if (unlikely(status < 0)) {
			XIO_DBG("send of connect failed, status = %d\n", status);
			goto done;
		}
	}
	if (status >= 0) {
		status = _request_info(output);
	}

done:
	if (status < 0) {
		XIO_INF("cannot connect to remote host '%s' (status = %d) -- retrying\n", output->host ? output->host : "NULL", status);
		_kill_socket(output);
	}
really_done:
	return status;
}

////////////////// own brick / input / output operations //////////////////

static int client_get_info(struct client_output *output, struct xio_info *info)
{
	int status;

	output->got_info = false;
	output->get_info = true;
	wake_up_interruptible(&output->event);

	wait_event_interruptible_timeout(output->info_event, output->got_info, 60 * HZ);
	status = -EIO;
	if (output->got_info && info) {
		memcpy(info, &output->info, sizeof(*info));
		status = 0;
	}

//done:
	return status;
}

static int client_io_get(struct client_output *output, struct aio_object *aio)
{
	int maxlen;

	if (aio->obj_initialized) {
		obj_get(aio);
		return aio->io_len;
	}

	/* Limit transfers to page boundaries.
	 * Currently, this is more restrictive than necessary.
	 * TODO: improve performance by doing better when possible.
	 * This needs help from the server in some efficient way.
	 */
	maxlen = PAGE_SIZE - (aio->io_pos & (PAGE_SIZE-1));
	if (aio->io_len > maxlen)
		aio->io_len = maxlen;

	if (!aio->io_data) { // buffered IO
		struct client_aio_aspect *aio_a = client_aio_get_aspect(output->brick, aio);
		if (!aio_a)
			return -EILSEQ;

		aio->io_data = brick_block_alloc(aio->io_pos, (aio_a->alloc_len = aio->io_len));

		aio_a->do_dealloc = true;
		aio->io_flags = 0;
	}

	obj_get_first(aio);
	return 0;
}

static void client_io_put(struct client_output *output, struct aio_object *aio)
{
	struct client_aio_aspect *aio_a;
	if (!obj_put(aio))
		return;
	aio_a = client_aio_get_aspect(output->brick, aio);
	if (aio_a && aio_a->do_dealloc) {
		brick_block_free(aio->io_data, aio_a->alloc_len);
	}
	obj_free(aio);
}

static
void _hash_insert(struct client_output *output, struct client_aio_aspect *aio_a)
{
	struct aio_object *aio = aio_a->object;
	int hash_index;

	spin_lock(&output->lock);
	list_del(&aio_a->io_head);
	list_add_tail(&aio_a->io_head, &output->aio_list);
	list_del(&aio_a->hash_head);
	aio->io_id = ++output->last_id;
	hash_index = aio->io_id % CLIENT_HASH_MAX;
	list_add_tail(&aio_a->hash_head, &output->hash_table[hash_index]);
	spin_unlock(&output->lock);
}

static void client_io_io(struct client_output *output, struct aio_object *aio)
{
	struct client_aio_aspect *aio_a;
	int error = -EINVAL;

	aio_a = client_aio_get_aspect(output->brick, aio);
	if (unlikely(!aio_a)) {
		goto error;
	}

	while (output->brick->max_flying > 0 && atomic_read(&output->fly_count) > output->brick->max_flying) {
		brick_msleep(1000 * 2 / HZ);
	}

	atomic_inc(&xio_global_io_flying);
	atomic_inc(&output->fly_count);
	obj_get(aio);

	aio_a->submit_jiffies = jiffies;
	_hash_insert(output, aio_a);

	wake_up_interruptible(&output->event);

	return;

error:
	XIO_ERR("IO error = %d\n", error);
	SIMPLE_CALLBACK(aio, error);
	client_io_put(output, aio);
}

static
int receiver_thread(void *data)
{
	struct client_output *output = data;
	int status = 0;

	while (!brick_thread_should_stop()) {
		struct xio_cmd cmd = {};
		struct list_head *tmp;
		struct client_aio_aspect *aio_a = NULL;
		struct aio_object *aio = NULL;

		status = xio_recv_struct(&output->socket, &cmd, xio_cmd_meta);
		if (status < 0)
			goto done;

		switch (cmd.cmd_code & CMD_FLAG_MASK) {
		case CMD_NOTIFY:
			local_trigger();
			break;
		case CMD_CONNECT:
			if (cmd.cmd_int1 < 0) {
				status = cmd.cmd_int1;
				XIO_ERR("at remote side: brick connect failed, remote status = %d\n", status);
				goto done;
			}
			break;
		case CMD_CB:
		{
			int hash_index = cmd.cmd_int1 % CLIENT_HASH_MAX;

			spin_lock(&output->lock);
			for (tmp = output->hash_table[hash_index].next; tmp != &output->hash_table[hash_index]; tmp = tmp->next) {
				struct aio_object *tmp_aio;
				aio_a = container_of(tmp, struct client_aio_aspect, hash_head);
				tmp_aio = aio_a->object;
				if (unlikely(!tmp_aio)) {
					spin_unlock(&output->lock);
					XIO_ERR("bad internal aio pointer\n");
					status = -EBADR;
					goto done;
				}
				if (tmp_aio->io_id == cmd.cmd_int1) {
					aio = tmp_aio;
					list_del_init(&aio_a->hash_head);
					list_del_init(&aio_a->io_head);
					break;
				}
			}
			spin_unlock(&output->lock);

			if (unlikely(!aio)) {
				XIO_WRN("got unknown id = %d for callback\n", cmd.cmd_int1);
				status = -EBADR;
				goto done;
			}

			status = xio_recv_cb(&output->socket, aio, &cmd);
			if (unlikely(status < 0)) {
				XIO_WRN("interrupted data transfer during callback, status = %d\n", status);
				_hash_insert(output, aio_a);
				goto done;
			}

			SIMPLE_CALLBACK(aio, 0);

			client_io_put(output, aio);

			atomic_dec(&output->fly_count);
			atomic_dec(&xio_global_io_flying);
			break;
		}
		case CMD_GETINFO:
			status = xio_recv_struct(&output->socket, &output->info, xio_info_meta);
			if (status < 0) {
				XIO_WRN("got bad info from remote side, status = %d\n", status);
				goto done;
			}
			output->got_info = true;
			wake_up_interruptible(&output->info_event);
			break;
		default:
			XIO_ERR("got bad command %d from remote side, terminating.\n", cmd.cmd_code);
			status = -EBADR;
			goto done;
		}
	done:
		brick_string_free(cmd.cmd_str1);
		if (unlikely(status < 0)) {
			if (!output->recv_error) {
				XIO_DBG("signalling status = %d\n", status);
				output->recv_error = status;
			}
			wake_up_interruptible(&output->event);
			brick_msleep(100);
		}
	}

	if (status < 0) {
		XIO_WRN("receiver thread terminated with status = %d, recv_error = %d\n", status, output->recv_error);
	}

	xio_shutdown_socket(&output->socket);
	wake_up_interruptible(&output->receiver.run_event);
	return status;
}

static
void _do_resubmit(struct client_output *output)
{
	spin_lock(&output->lock);
	if (!list_empty(&output->wait_list)) {
		struct list_head *first = output->wait_list.next;
		struct list_head *last = output->wait_list.prev;
		struct list_head *old_start = output->aio_list.next;
#define list_connect __list_del // the original routine has a misleading name: in reality it is more general
		list_connect(&output->aio_list, first);
		list_connect(last, old_start);
		INIT_LIST_HEAD(&output->wait_list);
	}
	spin_unlock(&output->lock);
}

static
void _do_timeout(struct client_output *output, struct list_head *anchor, bool force)
{
	struct client_brick *brick = output->brick;
	struct list_head *tmp;
	struct list_head *next;
	LIST_HEAD(tmp_list);
	int rounds = 0;
	long io_timeout = brick->io_timeout;

	if (io_timeout <= 0)
		io_timeout = global_net_io_timeout;

	if (!xio_net_is_alive)
		force = true;

	if (!force && io_timeout <= 0)
		return;

	io_timeout *= HZ;

	spin_lock(&output->lock);
	for (tmp = anchor->next, next = tmp->next; tmp != anchor; tmp = next, next = tmp->next) {
		struct client_aio_aspect *aio_a;

		aio_a = container_of(tmp, struct client_aio_aspect, io_head);

		if (!force &&
		    !time_is_before_jiffies(aio_a->submit_jiffies + io_timeout)) {
			continue;
		}

		list_del_init(&aio_a->hash_head);
		list_del_init(&aio_a->io_head);
		list_add_tail(&aio_a->tmp_head, &tmp_list);
	}
	spin_unlock(&output->lock);

	while (!list_empty(&tmp_list)) {
		struct client_aio_aspect *aio_a;
		struct aio_object *aio;

		tmp = tmp_list.next;
		list_del_init(tmp);
		aio_a = container_of(tmp, struct client_aio_aspect, tmp_head);
		aio = aio_a->object;

		if (!rounds++) {
			XIO_WRN("timeout after %ld: signalling IO error at pos = %lld len = %d\n",
				 io_timeout,
				 aio->io_pos,
				 aio->io_len);
		}

		atomic_inc(&output->timeout_count);

		SIMPLE_CALLBACK(aio, -ENOTCONN);

		client_io_put(output, aio);

		atomic_dec(&output->fly_count);
		atomic_dec(&xio_global_io_flying);
	}
}

static int sender_thread(void *data)
{
	struct client_output *output = data;
	struct client_brick *brick = output->brick;
	bool do_kill = false;
	int status = 0;

	output->receiver.restart_count = 0;

	while (!brick_thread_should_stop()) {
		struct list_head *tmp = NULL;
		struct client_aio_aspect *aio_a;
		struct aio_object *aio;

		if (unlikely(output->recv_error != 0 || !xio_socket_is_alive(&output->socket))) {
			XIO_DBG("recv_error = %d do_kill = %d\n", output->recv_error, do_kill);
			if (do_kill) {
				do_kill = false;
				_kill_socket(output);
				brick_msleep(3000);
			}

			status = _connect(output, brick->brick_name);
			if (unlikely(status < 0)) {
				brick_msleep(3000);
				_do_timeout(output, &output->wait_list, false);
				_do_timeout(output, &output->aio_list, false);
				continue;
			}
			brick->connection_state = 2;
			do_kill = true;
			/* Re-Submit any waiting requests
			 */
			_do_resubmit(output);
		}

		wait_event_interruptible_timeout(output->event,
						 !list_empty(&output->aio_list) ||
						 output->get_info ||
						 output->recv_error != 0 ||
						 brick_thread_should_stop(),
						 1 * HZ);

		if (unlikely(output->recv_error != 0)) {
			XIO_DBG("recv_error = %d\n", output->recv_error);
			brick_msleep(1000);
			continue;
		}

		if (output->get_info) {
			status = _request_info(output);
			if (status >= 0) {
				output->get_info = false;
			} else {
				XIO_WRN("cannot get info, status = %d\n", status);
				brick_msleep(1000);
			}
		}

		/* Grab the next aio from the queue
		 */
		spin_lock(&output->lock);
		if (list_empty(&output->aio_list)) {
			spin_unlock(&output->lock);
			continue;
		}
		tmp = output->aio_list.next;
		list_del(tmp);
		list_add(tmp, &output->wait_list);
		aio_a = container_of(tmp, struct client_aio_aspect, io_head);
		spin_unlock(&output->lock);

		aio = aio_a->object;

		if (brick->limit_mode) {
			int amount = 0;
			if (aio->io_cs_mode < 2)
				amount = (aio->io_len - 1) / 1024 + 1;
			xio_limit_sleep(&client_limiter, amount);
		}

		status = xio_send_aio(&output->socket, aio);
		if (unlikely(status < 0)) {
			// retry submission on next occasion..
			XIO_WRN("sending failed, status = %d\n", status);

			if (do_kill) {
				do_kill = false;
				_kill_socket(output);
			}
			_hash_insert(output, aio_a);
			brick_msleep(1000);
			continue;
		}
	}
//done:
	if (status < 0) {
		XIO_WRN("sender thread terminated with status = %d\n", status);
	}

	if (do_kill) {
		_kill_socket(output);
	}

	/* Signal error on all pending IO requests.
	 * We have no other chance (except probably delaying
	 * this until destruction which is probably not what
	 * we want).
	 */
	_do_timeout(output, &output->wait_list, true);
	_do_timeout(output, &output->aio_list, true);

	wake_up_interruptible(&output->sender.run_event);
	XIO_DBG("sender terminated\n");
	return status;
}

static int client_switch(struct client_brick *brick)
{
	struct client_output *output = brick->outputs[0];
	int status = 0;

	if (brick->power.button) {
		if (brick->power.led_on)
			goto done;
		xio_set_power_led_off((void*)brick, false);
		if (!output->sender.thread) {
			brick->connection_state = 1;
			output->sender.thread = brick_thread_create(sender_thread, output, "xio_sender%d", thread_count++);
			if (unlikely(!output->sender.thread)) {
				XIO_ERR("cannot start sender thread\n");
				status = -ENOENT;
				goto done;
			}
		}
		if (output->sender.thread) {
			xio_set_power_led_on((void*)brick, true);
		}
	} else {
		if (brick->power.led_off)
			goto done;
		xio_set_power_led_on((void*)brick, false);
		_kill_thread(&output->sender, "sender");
		brick->connection_state = 0;
		if (!output->sender.thread) {
			xio_set_power_led_off((void*)brick, !output->sender.thread);
		}
	}
done:
	return status;
}


//////////////// informational / statistics ///////////////

static
char *client_statistics(struct client_brick *brick, int verbose)
{
	struct client_output *output = brick->outputs[0];
	char *res = brick_string_alloc(1024);

	snprintf(res, 1024,
		 "#%d socket "
		 "max_flying = %d "
		 "io_timeout = %d | "
		 "timeout_count = %d "
		 "fly_count = %d\n",
		 output->socket.s_debug_nr,
		 brick->max_flying,
		 brick->io_timeout,
		 atomic_read(&output->timeout_count),
		 atomic_read(&output->fly_count));

	return res;
}

static
void client_reset_statistics(struct client_brick *brick)
{
	struct client_output *output = brick->outputs[0];
	atomic_set(&output->timeout_count, 0);
}

//////////////// object / aspect constructors / destructors ///////////////

static int client_aio_aspect_init_fn(struct generic_aspect *_ini)
{
	struct client_aio_aspect *ini = (void*)_ini;
	INIT_LIST_HEAD(&ini->io_head);
	INIT_LIST_HEAD(&ini->hash_head);
	INIT_LIST_HEAD(&ini->tmp_head);
	return 0;
}

static void client_aio_aspect_exit_fn(struct generic_aspect *_ini)
{
	struct client_aio_aspect *ini = (void*)_ini;
	CHECK_HEAD_EMPTY(&ini->io_head);
	CHECK_HEAD_EMPTY(&ini->hash_head);
}

XIO_MAKE_STATICS(client);

////////////////////// brick constructors / destructors ////////////////////

static int client_brick_construct(struct client_brick *brick)
{
	return 0;
}

static int client_output_construct(struct client_output *output)
{
	int i;

	output->hash_table = brick_block_alloc(0, PAGE_SIZE);

	for (i = 0; i < CLIENT_HASH_MAX; i++) {
		INIT_LIST_HEAD(&output->hash_table[i]);
	}
	spin_lock_init(&output->lock);
	INIT_LIST_HEAD(&output->aio_list);
	INIT_LIST_HEAD(&output->wait_list);
	init_waitqueue_head(&output->event);
	init_waitqueue_head(&output->sender.run_event);
	init_waitqueue_head(&output->receiver.run_event);
	init_waitqueue_head(&output->info_event);
	return 0;
}

static int client_output_destruct(struct client_output *output)
{
	if (output->path) {
		brick_string_free(output->path);
		output->path = NULL;
	}
	brick_block_free(output->hash_table, PAGE_SIZE);
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct client_brick_ops client_brick_ops = {
	.brick_switch = client_switch,
	.brick_statistics = client_statistics,
	.reset_statistics = client_reset_statistics,
};

static struct client_output_ops client_output_ops = {
	.xio_get_info = client_get_info,
	.aio_get = client_io_get,
	.aio_put = client_io_put,
	.aio_io = client_io_io,
};

const struct client_input_type client_input_type = {
	.type_name = "client_input",
	.input_size = sizeof(struct client_input),
};

static const struct client_input_type *client_input_types[] = {
	&client_input_type,
};

const struct client_output_type client_output_type = {
	.type_name = "client_output",
	.output_size = sizeof(struct client_output),
	.master_ops = &client_output_ops,
	.output_construct = &client_output_construct,
	.output_destruct = &client_output_destruct,
};

static const struct client_output_type *client_output_types[] = {
	&client_output_type,
};

const struct client_brick_type client_brick_type = {
	.type_name = "client_brick",
	.brick_size = sizeof(struct client_brick),
	.max_inputs = 0,
	.max_outputs = 1,
	.master_ops = &client_brick_ops,
	.aspect_types = client_aspect_types,
	.default_input_types = client_input_types,
	.default_output_types = client_output_types,
	.brick_construct = &client_brick_construct,
};
EXPORT_SYMBOL_GPL(client_brick_type);

////////////////// module init stuff /////////////////////////

struct xio_limiter client_limiter = {
	.lim_max_rate = 0,
};
EXPORT_SYMBOL_GPL(client_limiter);

int global_net_io_timeout = CONFIG_MARS_NETIO_TIMEOUT;
EXPORT_SYMBOL_GPL(global_net_io_timeout);

int __init init_xio_client(void)
{
	XIO_INF("init_client()\n");
	_client_brick_type = (void*)&client_brick_type;
	return client_register_brick_type();
}

void __exit exit_xio_client(void)
{
	XIO_INF("exit_client()\n");
	client_unregister_brick_type();
}
