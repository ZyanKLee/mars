// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING
//#define IO_DEBUGGING
//#define STAT_DEBUGGING // here means: display full statistics

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/debug_locks.h>

#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#define _STRATEGY
#include "mars.h"

#include <linux/kthread.h>
#include <linux/wait.h>

// used brick types
#include "mars_server.h"
#include "mars_client.h"
#include "mars_copy.h"
#include "mars_aio.h"
#include "mars_trans_logger.h"
#include "mars_if.h"

static struct task_struct *main_thread = NULL;

typedef int (*light_worker_fn)(void *buf, struct mars_dent *dent);

struct light_class {
	char *cl_name;
	int    cl_len;
	char   cl_type;
	bool   cl_hostcontext;
	bool   cl_serial;
	int    cl_father;
	light_worker_fn cl_forward;
	light_worker_fn cl_backward;
};

///////////////////////////////////////////////////////////////////////

// internal helpers

#define MARS_DELIM ','

static int _parse_args(struct mars_dent *dent, char *str, int count)
{
	int i;
	int status = -EINVAL;
	if (!str)
		goto done;
	if (!dent->d_args) {
		dent->d_args = kstrdup(str, GFP_MARS);
		if (!dent->d_args) {
			status = -ENOMEM;
			goto done;
		}
	}
	for (i = 0; i < count; i++) {
		char *tmp;
		int len;
		if (!*str)
			goto done;
		if (i == count-1) {
			len = strlen(str);
		} else {
			char *tmp = strchr(str, MARS_DELIM);
			if (!tmp)
				goto done;
			len = (tmp - str);
		}
		tmp = kzalloc(len+1, GFP_MARS);
		if (!tmp) {
			status = -ENOMEM;
			goto done;
		}
		if (dent->d_argv[i]) {
			kfree(dent->d_argv[i]);
		}
		dent->d_argv[i] = tmp;
		strncpy(dent->d_argv[i], str, len);
		dent->d_argv[i][len] = '\0';

		str += len;
		if (i != count-1)
			str++;
	}
	status = 0;
done:
	if (status < 0) {
		MARS_ERR("bad syntax '%s' (should have %d args), status = %d\n", dent->d_args ? dent->d_args : "", count, status);
	}
	return status;
}

static
char *_backskip_replace(const char *path, char delim, bool insert, const char *fmt, ...)
{
	int path_len = strlen(path);
	int total_len = strlen(fmt) + path_len + MARS_PATH_MAX;
	char *res = kmalloc(total_len, GFP_MARS);
	if (likely(res)) {
		va_list args;
		int pos = path_len;
		int plus;

		while (pos > 0 && path[pos] != delim) {
			pos--;
		}
		memcpy(res, path, pos);

		va_start(args, fmt);
		plus = vsnprintf(res + pos, total_len - pos, fmt, args);
		va_end(args);

		if (insert) {
			strncpy(res + pos + plus, path + pos + 1, total_len - pos - plus);
		}
	}
	return res;
}

///////////////////////////////////////////////////////////////////////

static
int __make_copy(
		struct mars_global *global,
		struct mars_dent *belongs,
		const char *path,
		const char *parent,
		const char *argv[],
		loff_t start_pos, // -1 means at EOF
		struct copy_brick **__copy)
{
	struct mars_brick *copy;
	struct copy_brick *_copy;
	const char *fullpath[2] = {};
	struct mars_output *output[2] = {};
	struct mars_info info[2] = {};
	int i;
	int status = -1;

	for (i = 0; i < 2; i++) {
		struct mars_brick *aio;

		if (parent) {
			fullpath[i] = path_make("%s/%s", parent, argv[i]);
			if (!fullpath[i]) {
				MARS_ERR("cannot make path '%s/%s'\n", parent, argv[i]);
				goto done;
			}
		} else {
			fullpath[i] = argv[i];
		}

		aio =
			make_brick_all(global,
				       NULL,
				       10 * HZ,
				       NULL,
				       (const struct generic_brick_type*)&aio_brick_type,
				       (const struct generic_brick_type*[]){},
				       fullpath[i],
				       (const char *[]){},
				       0);
		if (!aio) {
			MARS_DBG("cannot instantiate '%s'\n", fullpath[i]);
			goto done;
		}
		output[i] = aio->outputs[0];
	}

	copy =
		make_brick_all(global,
			       belongs,
			       0,
			       path,
			       (const struct generic_brick_type*)&copy_brick_type,
			       (const struct generic_brick_type*[]){NULL,NULL,NULL,NULL},
			       "%s",
			       (const char *[]){"%s", "%s", "%s", "%s"},
			       4,
			       path,
			       fullpath[0],
			       fullpath[0],
			       fullpath[1],
			       fullpath[1]);
	if (!copy) {
		MARS_DBG("fail '%s'\n", path);
		goto done;
	}
	copy->status_level = 2;
	_copy = (void*)copy;
	if (__copy)
		*__copy = _copy;

	/* Determine the copy area, switch on when necessary
	 */
	if (!copy->power.button && copy->power.led_off) {
		for (i = 0; i < 2; i++) {
			status = output[i]->ops->mars_get_info(output[i], &info[i]);
			if (status < 0) {
				MARS_ERR("cannot determine current size of '%s'\n", argv[i]);
				goto done;
			}
			MARS_DBG("%d '%s' current_size = %lld\n", i, fullpath[i], info[i].current_size);
		}
		_copy->copy_start = info[1].current_size;
		if (start_pos != -1) {
			_copy->copy_start = start_pos;
			if (unlikely(info[0].current_size != info[1].current_size)) {
				MARS_ERR("oops, devices have different size %lld != %lld at '%s'\n", info[0].current_size, info[1].current_size, path);
				status = -EINVAL;
				goto done;
			}
			if (unlikely(start_pos > info[0].current_size)) {
				MARS_ERR("bad start position %lld is larger than actual size %lld on '%s'\n", start_pos, info[0].current_size, path);
				status = -EINVAL;
				goto done;
			}
		}
		MARS_DBG("copy_start = %lld\n", _copy->copy_start);
		_copy->copy_end = info[0].current_size;
		MARS_DBG("copy_end = %lld\n", _copy->copy_end);
		if (_copy->copy_start < _copy->copy_end) {
			status = mars_power_button_recursive((void*)copy, true, 10 * HZ);
			MARS_DBG("copy switch status = %d\n", status);
		}
	}
	status = 0;

done:
	MARS_DBG("status = %d\n", status);
	for (i = 0; i < 2; i++) {
		if (fullpath[i] && fullpath[i] != argv[i])
			kfree(fullpath[i]);
	}
	return status;
}

///////////////////////////////////////////////////////////////////////

// remote workers

struct mars_peerinfo {
	struct mars_global *global;
	char *peer;
	char *path;
	struct socket *socket;
	struct task_struct *thread;
	wait_queue_head_t event;
	light_worker_fn worker;
	int maxdepth;
};

static
int _update_file(struct mars_global *global, const char *path, const char *peer, loff_t end_pos)
{
	const char *tmp = path_make("%s@%s", path, peer);
	const char *argv[2] = { tmp, path };
	struct copy_brick *copy = NULL;
	int status = -ENOMEM;

	if (unlikely(!tmp))
		goto done;

	MARS_DBG("tmp = '%s' path = '%s'\n", tmp, path);
	status = __make_copy(global, NULL, path, NULL, argv, -1, &copy);
	if (status >= 0 && copy && !copy->permanent_update) {
		if (end_pos > copy->copy_end) {
			MARS_DBG("appending to '%s' %lld => %lld\n", path, copy->copy_end, end_pos);
			copy->copy_end = end_pos;
		}
	}

done:
	if (tmp)
		kfree(tmp);
	return status;
}

static
int _is_peer_logfile(const char *name, const char *id)
{
	int len = strlen(name);
	int idlen = id ? strlen(id) : 4 + 9 + 1;

	if (len <= idlen ||
	   strncmp(name, "log-", 4) != 0) {
		MARS_DBG("not a logfile at all: '%s'\n", name);
		return false;
	}
	if (id &&
	   name[len - idlen - 1] == '-' &&
	   strncmp(name + len - idlen, id, idlen) == 0) {
		MARS_DBG("not a peer logfile: '%s'\n", name);
		return false;
	}
	MARS_DBG("found peer logfile: '%s'\n", name);
	return true;
}

static
int run_bones(void *buf, struct mars_dent *dent)
{
	int status = 0;
	struct mars_peerinfo *peer = buf;
	struct kstat local_stat = {};
	bool stat_ok;
	bool update_mtime = true;
	bool update_ctime = true;

	if (!strncmp(dent->d_name, ".tmp", 4)) {
		goto done;
	}
	if (!strncmp(dent->d_name, "ignore", 6)) {
		goto done;
	}

	status = mars_lstat(dent->d_path, &local_stat);
	stat_ok = (status >= 0);

	if (stat_ok) {
		update_mtime = timespec_compare(&dent->new_stat.mtime, &local_stat.mtime) > 0;
		update_ctime = timespec_compare(&dent->new_stat.ctime, &local_stat.ctime) > 0;

		//MARS_DBG("timestamps '%s' remote = %ld.%09ld local = %ld.%09ld\n", dent->d_path, dent->new_stat.mtime.tv_sec, dent->new_stat.mtime.tv_nsec, local_stat.mtime.tv_sec, local_stat.mtime.tv_nsec);

		if ((dent->new_stat.mode & S_IRWXU) !=
		   (local_stat.mode & S_IRWXU) &&
		   update_ctime) {
			mode_t newmode = local_stat.mode;
			MARS_DBG("chmod '%s' 0x%xd -> 0x%xd\n", dent->d_path, newmode & S_IRWXU, dent->new_stat.mode & S_IRWXU);
			newmode &= ~S_IRWXU;
			newmode |= (dent->new_stat.mode & S_IRWXU);
			mars_chmod(dent->d_path, newmode);
			mars_trigger();
		}

		if (dent->new_stat.uid != local_stat.uid && update_ctime) {
			MARS_DBG("lchown '%s' %d -> %d\n", dent->d_path, local_stat.uid, dent->new_stat.uid);
			mars_lchown(dent->d_path, dent->new_stat.uid);
			mars_trigger();
		}
	}

	if (S_ISDIR(dent->new_stat.mode)) {
		if (strncmp(dent->d_name, "resource-", 9)) {
			MARS_DBG("ignoring directory '%s'\n", dent->d_path);
			goto done;
		}
		if (!stat_ok) {
			status = mars_mkdir(dent->d_path);
			MARS_DBG("create directory '%s' status = %d\n", dent->d_path, status);
			mars_chmod(dent->d_path, dent->new_stat.mode);
			mars_lchown(dent->d_path, dent->new_stat.uid);
		}
	} else if (S_ISLNK(dent->new_stat.mode) && dent->new_link) {
		if (!stat_ok || update_mtime) {
			status = mars_symlink(dent->new_link, dent->d_path, &dent->new_stat.mtime, dent->new_stat.uid);
			MARS_DBG("create symlink '%s' -> '%s' status = %d\n", dent->d_path, dent->new_link, status);
			mars_trigger();
		}
	} else if (S_ISREG(dent->new_stat.mode) && _is_peer_logfile(dent->d_name, my_id())) {
		loff_t src_size = dent->new_stat.size;

		if (!stat_ok || local_stat.size < src_size) {
			// check whether connect is allowed
			char *connect_path = _backskip_replace(dent->d_path, '/', false, "/connect-%s", my_id());
			if (likely(connect_path)) {
				struct kstat connect_stat = {};
				status = mars_lstat(connect_path, &connect_stat);
				MARS_DBG("connect_path = '%s' stat status = %d uid = %d\n", connect_path, status, connect_stat.uid);
				kfree(connect_path);
				if (status >= 0 && !connect_stat.uid) {
					// parent is not available here
					status = _update_file(peer->global, dent->d_path, peer->peer, src_size);
					MARS_DBG("update '%s' from peer '%s' status = %d\n", dent->d_path, peer->peer, status);
					if (status >= 0) {
						char *tmp = _backskip_replace(dent->d_path, '-', false, "-%s", my_id());
						status = -ENOMEM;
						if (likely(tmp)) {
							struct mars_dent *local_alias;
							status = 0;
							MARS_DBG("local alias for '%s' is '%s'\n", dent->d_path, tmp);
							local_alias = mars_find_dent((void*)peer->global, tmp);
							if (!local_alias) {
								status = mars_symlink(dent->d_name, tmp, &dent->new_stat.mtime, 0);
								MARS_DBG("create alias '%s' -> '%s' status = %d\n", tmp, dent->d_name, status);
								mars_trigger();
							}
							kfree(tmp);
						}
					}
				}
			}
		}
	} else {
		MARS_DBG("ignoring '%s'\n", dent->d_path);
	}

 done:
	return status;
}

///////////////////////////////////////////////////////////////////////

// remote working infrastructure

static
void _peer_cleanup(struct mars_peerinfo *peer)
{
	if (peer->socket) {
		kernel_sock_shutdown(peer->socket, SHUT_WR);
		peer->socket = NULL;
	}
	//...
}

static
int remote_thread(void *data)
{
	struct mars_peerinfo *peer = data;
	char *real_peer;
	struct sockaddr_storage sockaddr = {};
	int status;

	if (!peer)
		return -1;

	real_peer = mars_translate_hostname(peer->global, peer->peer);
	MARS_INF("-------- remote thread starting on peer '%s' (%s)\n", peer->peer, real_peer);

	//fake_mm();

	status = mars_create_sockaddr(&sockaddr, real_peer);
	if (unlikely(status < 0)) {
		MARS_ERR("unusable remote address '%s' (%s)\n", real_peer, peer->peer);
		goto done;
	}

        while (!kthread_should_stop()) {
		LIST_HEAD(tmp_list);
		struct mars_cmd cmd = {
			.cmd_code = CMD_GETENTS,
			.cmd_str1 = peer->path,
			.cmd_int1 = peer->maxdepth,
		};

		if (!peer->socket) {
			status = mars_create_socket(&peer->socket, &sockaddr, false);
			if (unlikely(status < 0)) {
				peer->socket = NULL;
				MARS_INF("no connection to '%s'\n", real_peer);
				msleep(5000);
				continue;
			}
			MARS_DBG("successfully opened socket to '%s'\n", real_peer);
			continue;
		}

		status = mars_send_struct(&peer->socket, &cmd, mars_cmd_meta);
		if (unlikely(status < 0)) {
			MARS_ERR("communication error on send, status = %d\n", status);
			_peer_cleanup(peer);
			msleep(5000);
			continue;
		}

		status = mars_recv_dent_list(&peer->socket, &tmp_list);
		if (unlikely(status < 0)) {
			MARS_ERR("communication error on receive, status = %d\n", status);
			_peer_cleanup(peer);
			msleep(5000);
			continue;
		}

		//MARS_DBG("AHA!!!!!!!!!!!!!!!!!!!!\n");

		{
			struct list_head *tmp;
			for (tmp = tmp_list.next; tmp != &tmp_list; tmp = tmp->next) {
				struct mars_dent *dent = container_of(tmp, struct mars_dent, dent_link);
				if (!dent->d_path) {
					MARS_DBG("NULL\n");
					continue;
				}
				//MARS_DBG("path = '%s'\n", dent->d_path);
				if (!peer->worker)
					continue;
				status = peer->worker(peer, dent);
				//MARS_DBG("path = '%s' worker status = %d\n", dent->d_path, status);
			}
		}

		mars_free_dent_all(&tmp_list);

		if (!kthread_should_stop())
			msleep(10 * 1000);
	}

	MARS_INF("-------- remote thread terminating\n");

	_peer_cleanup(peer);

done:
	//cleanup_mm();
	peer->thread = NULL;
	if (real_peer)
		kfree(real_peer);
	return 0;
}

///////////////////////////////////////////////////////////////////////

// helpers for worker functions

static int _kill_peer(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_peerinfo *peer = dent->d_private;

	if (global->global_power.button) {
		return 0;
	}
	if (!peer) {
		return 0;
	}
	if (!peer->thread) {
		MARS_ERR("oops, remote thread is not running - doing cleanup myself\n");
		_peer_cleanup(peer);
		dent->d_private = NULL;
		return -1;

	}
	kthread_stop(peer->thread);
	dent->d_private = NULL;
	return 0;
}

static int _make_peer(void *buf, struct mars_dent *dent, char *mypeer, char *path, light_worker_fn worker)
{
	static int serial = 0;
	struct mars_global *global = buf;
	struct mars_peerinfo *peer;
	int status = 0;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}
	if (!mypeer) {
		status = _parse_args(dent, dent->new_link, 1);
		if (status < 0)
			goto done;
		mypeer = dent->d_argv[0];
	}

	MARS_DBG("peer '%s'\n", mypeer);
	if (!dent->d_private) {
		dent->d_private = kzalloc(sizeof(struct mars_peerinfo), GFP_MARS);
		if (!dent->d_private) {
			MARS_ERR("no memory for peer structure\n");
			return -1;
		}

		peer = dent->d_private;
		peer->global = global;
		peer->peer = mypeer;
		peer->path = path;
		peer->worker = worker;
		peer->maxdepth = 2;
		init_waitqueue_head(&peer->event);
	}
	peer = dent->d_private;
	if (!peer->thread) {
		peer->thread = kthread_create(remote_thread, peer, "mars_remote%d", serial++);
		if (unlikely(IS_ERR(peer->thread))) {
			MARS_ERR("cannot start peer thread, status = %d\n", (int)PTR_ERR(peer->thread));
			peer->thread = NULL;
			return -1;
		}
		MARS_DBG("starting peer thread\n");
		wake_up_process(peer->thread);
	}

done:
	return status;
}

static int _kill_remote(void *buf, struct mars_dent *dent)
{
	return _kill_peer(buf, dent);
}

static int _make_remote(void *buf, struct mars_dent *dent)
{
	return _make_peer(buf, dent, NULL, "/mars", NULL);
}

static int kill_scan(void *buf, struct mars_dent *dent)
{
	return _kill_peer(buf, dent);
}

static int make_scan(void *buf, struct mars_dent *dent)
{
	MARS_DBG("path = '%s' peer = '%s'\n", dent->d_path, dent->d_rest);
	if (!strcmp(dent->d_rest, my_id())) {
		return 0;
	}
	return _make_peer(buf, dent, dent->d_rest, "/mars", run_bones);
}


static
int kill_all(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;

	if (global->global_power.button &&
	   (!dent->d_kill_inactive || dent->d_activate)) {
		return 0;
	}
	MARS_DBG("killing dent = '%s'\n", dent->d_path);
	//mars_kill_dent(dent);
	return 0;
}

///////////////////////////////////////////////////////////////////////

// handlers / helpers for logfile rotation

struct mars_rotate {
	struct mars_global *global;
	struct mars_dent *replay_link;
	struct mars_dent *aio_dent;
	struct aio_brick *aio_brick;
	struct mars_info aio_info;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *relevant_log;
	struct mars_brick *relevant_brick;
	struct mars_dent *current_log;
	struct mars_dent *prev_log;
	struct mars_dent *next_log;
	long long last_jiffies;
	loff_t start_pos;
	loff_t end_pos;
	int max_sequence;
	bool has_error;
	bool do_replay;
	bool is_primary;
};

static
void _create_new_logfile(char *path)
{
	struct file *f;
	const int flags = O_RDWR | O_CREAT | O_EXCL;
	const int prot = 0600;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	f = filp_open(path, flags, prot);
	set_fs(oldfs);
	if (IS_ERR(f)) {
		MARS_ERR("could not create logfile '%s' status = %d\n", path, (int)PTR_ERR(f));
	} else {
		MARS_DBG("created empty logfile '%s'\n", path);
		filp_close(f, NULL);
		mars_trigger();
	}
}

static
int _update_replaylink(struct mars_rotate *rot, struct mars_dent *parent, int sequence, loff_t pos, bool check_exist)
{
	struct timespec now = {};
	char *old;
	char *new;
	int status = -ENOMEM;

	if (check_exist) {
		struct kstat kstat;
		char *test = path_make("%s/log-%09d-%s", parent->d_path, sequence, my_id());
		if (!test) {
			goto out_old;
		}
		status = mars_lstat(test, &kstat);
		kfree(test);
		if (status < 0) {
			MARS_DBG("could not update replay link to nonexisting logfile %09d\n", sequence);
			goto out_old;
		}
		status = -ENOMEM;
	}

	old = path_make("log-%09d-%s,%lld", sequence, my_id(), pos);
	if (!old) {
		goto out_old;
	}
	new = path_make("%s/replay-%s", parent->d_path, my_id());
	if (!new) {
		goto out_new;
	}

	get_lamport(&now);
	status = mars_symlink(old, new, &now, 0);
	if (status < 0) {
		MARS_ERR("cannot create symlink '%s' -> '%s' status = %d\n", old, new, status);
	} else {
		MARS_DBG("make symlink '%s' -> '%s' status = %d\n", old, new, status);
	}
	if (status >= 0) {
		rot->last_jiffies = jiffies;
	}

	kfree(new);
out_new:
	kfree(old);
out_old:
	return status;
}

/* This must be called once at every round of logfile checking.
 */
static
int make_log_init(void *buf, struct mars_dent *parent)
{
	struct mars_global *global = buf;
	struct mars_brick *aio_brick;
	struct mars_brick *trans_brick;
	struct mars_rotate *rot = parent->d_private;
	struct mars_dent *replay_link;
	struct mars_dent *aio_dent;
	struct mars_output *output;
	char *replay_path = NULL;
	char *aio_path = NULL;
	int status;

	if (!rot) {
		rot = kzalloc(sizeof(struct mars_rotate), GFP_MARS);
		parent->d_private = rot;
		if (!rot) {
			MARS_ERR("cannot allocate rot structure\n");
			status = -ENOMEM;
			goto done;
		}
		rot->global = global;
	}

	rot->replay_link = NULL;
	rot->aio_dent = NULL;
	rot->aio_brick = NULL;
	rot->relevant_log = NULL;
	rot->relevant_brick = NULL;
	rot->prev_log = NULL;
	rot->next_log = NULL;
	rot->max_sequence = 0;
	rot->has_error = false;

	/* Fetch the replay status symlink.
	 * It must exist, and its value will control everything.
	 */
	replay_path = path_make("%s/replay-%s", parent->d_path, my_id());
	if (unlikely(!replay_path)) {
		MARS_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	replay_link = (void*)mars_find_dent(global, replay_path);
	if (unlikely(!replay_link || !replay_link->new_link)) {
		MARS_ERR("replay status symlink '%s' does not exist (%p)\n", replay_path, replay_link);
		status = -ENOENT;
		goto done;
	}

	status = _parse_args(replay_link, replay_link->new_link, 2);
	if (unlikely(status < 0)) {
		goto done;
	}
	rot->replay_link = replay_link;

	/* Fetch the referenced AIO dentry.
	 */
	aio_path = path_make("%s/%s", parent->d_path, replay_link->d_argv[0]);
	if (unlikely(!aio_path)) {
		MARS_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	aio_dent = (void*)mars_find_dent(global, aio_path);
	if (unlikely(!aio_dent)) {
		MARS_ERR("logfile '%s' does not exist\n", aio_path);
		status = -ENOENT;
		if (rot->is_primary) { // try to create an empty logfile
			_create_new_logfile(aio_path);
		}
		goto done;
	}
	rot->aio_dent = aio_dent;

	/* Fetch / make the AIO brick instance
	 */
	aio_brick =
		make_brick_all(global,
			       aio_dent,
			       10 * HZ,
			       aio_path,
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       "%s/%s",
			       (const char *[]){},
			       0,
			       parent->d_path,
			       replay_link->d_argv[0]);
	if (!aio_brick) {
		MARS_ERR("cannot access '%s'\n", aio_path);
		status = -EIO;
		goto done;
	}
	rot->aio_brick = (void*)aio_brick;

	/* Fetch the actual logfile size
	 */
	output = aio_brick->outputs[0];
	status = output->ops->mars_get_info(output, &rot->aio_info);
	if (status < 0) {
		MARS_ERR("cannot get info on '%s'\n", aio_path);
		goto done;
	}
	MARS_DBG("logfile '%s' size = %lld\n", aio_path, rot->aio_info.current_size);

	/* Fetch / make the transaction logger.
	 * We deliberately "forget" to connect the log input here.
	 * Will be carried out later in make_log().
	 * The final switch-on will be started in make_log_finalize().
	 */
	trans_brick =
		make_brick_all(global,
			       parent,
			       0,
			       aio_path,
			       (const struct generic_brick_type*)&trans_logger_brick_type,
			       (const struct generic_brick_type*[]){(const struct generic_brick_type*)&aio_brick_type},
			       "%s/logger", 
			       (const char *[]){"%s/data-%s"},
			       1,
			       parent->d_path,
			       parent->d_path,
			       my_id());
	status = -ENOENT;
	if (!trans_brick) {
		goto done;
	}
	rot->trans_brick = (void*)trans_brick;
	/* For safety, default is to try an (unnecessary) replay in case
	 * something goes wrong later.
	 */
	rot->do_replay = true;

	status = 0;

done:
	if (aio_path)
		kfree(aio_path);
	if (replay_path)
		kfree(replay_path);
	return status;
}


/* Internal helper. Return codes:
 * ret < 0 : error
 * ret == 0 : not relevant
 * ret == 1 : relevant, no transaction replay
 * ret == 2 : relevant for transaction replay
 * ret == 3 : relevant for appending
 */
static
int _check_logging_status(struct mars_global *global, struct mars_dent *dent, long long *oldpos, long long *newpos)
{
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = parent->d_private;
	int status = -EINVAL;

	CHECK_PTR(rot, done);

	status = 0;
	if (!rot->replay_link || !rot->aio_dent || !rot->aio_brick) {
		//MARS_DBG("nothing to do on '%s'\n", dent->d_path);
		goto done;
	}

	if (rot->aio_dent->d_serial != dent->d_serial) {
		//MARS_DBG("serial number %d not relevant\n", dent->d_serial);
		goto done;
	}

	if (sscanf(rot->replay_link->d_argv[1], "%lld", oldpos) != 1) {
		MARS_ERR("bad position argument '%s'\n", rot->replay_link->d_argv[1]);
		status = -EINVAL;
		goto done;
	}

	if (unlikely(rot->aio_info.current_size < *oldpos)) {
		MARS_ERR("oops, bad replay position attempted in logfile '%s' (file length %lld should never be smaller than requested position %lld, is your filesystem corrupted?) => please repair this by hand\n", rot->aio_dent->d_path, rot->aio_info.current_size, *oldpos);
		status = -EINVAL;
		goto done;
	}

	if (rot->aio_info.current_size > *oldpos) {
		MARS_DBG("transaction log replay is necessary on '%s' from %lld to %lld\n", rot->aio_dent->d_path, *oldpos, rot->aio_info.current_size);
		*newpos = rot->aio_info.current_size;
		status = 2;
	} else if (rot->aio_info.current_size > 0) {
		MARS_DBG("transaction log '%s' is already applied (would be usable for appending at position %lld, but a fresh log is needed for safety reasons)\n", rot->aio_dent->d_path, *oldpos);
		*newpos = rot->aio_info.current_size;
		status = 1;
	} else if (!rot->is_primary) {
		MARS_DBG("empty transaction log '%s' would be usable, but I am not primary\n", rot->aio_dent->d_path);
		status = 0;
	} else {
		MARS_DBG("empty transaction log '%s' is usable for me as a primary node\n", rot->aio_dent->d_path);
		status = 3;
	}

done:
	return status;
}


/* Note: this is strictly called in d_serial order.
 * This is important!
 */
static
int make_log(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = parent->d_private;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *prev_log;
	loff_t start_pos = 0;
	loff_t end_pos = 0;
	int status = -EINVAL;

	CHECK_PTR(rot, err);

	status = 0;
	trans_brick = rot->trans_brick;
	if (!global->global_power.button || !dent->d_parent || !trans_brick || rot->has_error) {
		MARS_DBG("nothing to do rot_error = %d\n", rot->has_error);
		goto done;
	}

	if (dent->d_serial > rot->max_sequence) {
		rot->max_sequence = dent->d_serial;
	}

	/* Check for consecutiveness of logfiles
	 */
	prev_log = rot->next_log;
	if (prev_log && prev_log->d_serial + 1 != dent->d_serial) {
		MARS_ERR("transaction logs are not consecutive at '%s' (%d ~> %d)\n", dent->d_path, prev_log->d_serial, dent->d_serial);
		status = -EINVAL;
		goto done;
	}

	/* Skip any logfiles after the relevant one.
	 * This should happen only when replaying multiple logfiles
	 * in sequence, or when starting a new logfile for writing.
	 */
	if (rot->relevant_log) {
		if (rot->do_replay) {
			status = 0;
			goto ok;
		}
		if (rot->aio_dent->new_stat.size > 0) {
			MARS_ERR("oops, the new logfile '%s' is not empty -- for safety, I will not use it -- log rotation is disabled now\n", dent->d_path);
			status = -EINVAL;
			goto done;
		}
		MARS_DBG("considering next logfile '%s' for rotation\n", dent->d_path);
	}

	/* Find current logging status.
	 */
	status = _check_logging_status(global, dent, &start_pos, &end_pos);
	if (status < 0) {
		goto done;
	}
	/* Relevant or not?
	 */
	switch (status) {
	case 0: // not relevant
		goto ok;
	case 1: /* Relevant, but transaction replay already finished.
		 * When primary, switch over to a new logfile.
		 */
		if (!trans_brick->power.button && !trans_brick->power.led_on && trans_brick->power.led_off) {
			_update_replaylink(rot, dent->d_parent, dent->d_serial + 1, 0, !rot->is_primary);
			mars_trigger();
		}
		status = -EAGAIN;
		goto done;
	case 2: // relevant for transaction replay
		MARS_DBG("replaying transaction log '%s' from %lld to %lld\n", dent->d_path, start_pos, end_pos);
		rot->do_replay = true;
		rot->start_pos = start_pos;
		rot->end_pos = end_pos;
		rot->relevant_log = dent;
		break;
	case 3: // relevant for appending
		MARS_DBG("appending to transaction log '%s'\n", dent->d_path);
		rot->do_replay = false;
		rot->start_pos = 0;
		rot->end_pos = 0;
		rot->relevant_log = dent;
		break;
	default:
		MARS_ERR("bad internal status %d\n", status);
		status = -EINVAL;
		goto done;
	}

ok:
	/* All ok: switch over the indicators.
	 */
	rot->prev_log = rot->next_log;
	rot->next_log = dent;

done:
	if (status < 0) {
		MARS_DBG("rot_error status = %d\n", status);
		rot->has_error = true;
	}
err:
	return status;
}

static
int _start_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	int status = 0;

	if (trans_brick->power.button || !trans_brick->power.led_off) {
		goto done;
	}

	/* Internal safety checks
	 */
	status = -EINVAL;
	if (unlikely(!rot->aio_brick || !rot->relevant_log)) {
		MARS_ERR("something is missing, this should not happen\n");
		goto done;
	}
	if (unlikely(rot->relevant_brick)) {
		MARS_ERR("log aio brick already present, this should not happen\n");
		goto done;
	}

	/* For safety, disconnect old connection first
	 */
	if (trans_brick->inputs[1]->connect) {
		(void)generic_disconnect((void*)trans_brick->inputs[1]);
	}

	/* Open new transaction log
	 */
	rot->relevant_brick =
		make_brick_all(rot->global,
			       rot->relevant_log,
			       10 * HZ,
			       rot->relevant_log->d_path,
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       rot->relevant_log->d_path,
			       (const char *[]){},
			       0);
	if (!rot->relevant_brick) {
		MARS_ERR("log aio brick not open\n");
		goto done;
	}

	/* Connect to new transaction log
	 */
	status = generic_connect((void*)trans_brick->inputs[1], (void*)rot->relevant_brick->outputs[0]);
	if (status < 0) {
		goto done;
	}

	/* Supply all relevant parameters
	 */
	trans_brick->sequence = rot->relevant_log->d_serial;
	trans_brick->do_replay = rot->do_replay;
	trans_brick->current_pos = rot->start_pos;
	trans_brick->start_pos = rot->start_pos;
	trans_brick->end_pos = rot->end_pos;

	/* Switch on....
	 */
	status = mars_power_button((void*)trans_brick, true);
	MARS_DBG("status = %d\n", status);

done:
	return status;
}

static
int _stop_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	int status = 0;

	if (!trans_brick->power.button) {
		goto done;
	}

	/* Switch off....
	 */
	status = mars_power_button((void*)trans_brick, false);
	MARS_DBG("status = %d\n", status);
	if (status < 0) {
		goto done;
	}

	/* Disconnect old connection
	 */
	if (trans_brick->inputs[1]->connect && trans_brick->power.led_off) {
		(void)generic_disconnect((void*)trans_brick->inputs[1]);
	}

done:
	return status;
}

static
int make_log_finalize(struct mars_global *global, struct mars_dent *parent)
{
	struct mars_rotate *rot = parent->d_private;
	struct trans_logger_brick *trans_brick;
	int status = -EINVAL;

	CHECK_PTR(rot, done);

	trans_brick = rot->trans_brick;

	status = 0;
	if (!trans_brick) {
		MARS_DBG("nothing to do\n");
		goto done;
	}
	/* Stopping is also possible in case of errors
	 */
	if (trans_brick->power.button && trans_brick->power.led_on && !trans_brick->power.led_off) {
		bool do_stop =
			(rot->do_replay || trans_brick->do_replay)
			? (trans_brick->current_pos == trans_brick->end_pos)
			: (rot->relevant_log && rot->relevant_log != rot->current_log);
		MARS_DBG("do_stop = %d\n", (int)do_stop);

		if (do_stop || (long long)jiffies > rot->last_jiffies + 5 * HZ) {
			status = _update_replaylink(rot, parent, trans_brick->sequence, trans_brick->current_pos, true);
		}
		if (do_stop) {
			status = _stop_trans(rot);
		}
		goto done;
	}
	/* Special case: when no logfile exists,
	 * create one. This is an exception from the rule that
	 * normally userspace should control what happens in MARS.
	 */
	if (!rot->relevant_log && rot->is_primary && !rot->has_error && rot->max_sequence > 0) { // try to create an empty logfile
		char *tmp = path_make("%s/log-%09d-%s", parent->d_path, rot->max_sequence + 1, my_id());
		if (likely(tmp)) {
			_create_new_logfile(tmp);
			kfree(tmp);
			msleep(1000);
			goto done;
		}
	}
	/* Starting is only possible when no error ocurred.
	 */
	if (!rot->relevant_log || rot->has_error) {
		MARS_DBG("nothing to do\n");
		goto done;
	}

	/* Start when necessary
	 */
	if (!trans_brick->power.button && !trans_brick->power.led_on && trans_brick->power.led_off) {
		bool do_start = (!rot->do_replay || rot->start_pos != rot->end_pos);
		MARS_DBG("do_start = %d\n", (int)do_start);

		if (do_start) {
			status = _start_trans(rot);
			rot->current_log = rot->relevant_log;
		}
	} else {
		MARS_DBG("trans_brick %d %d %d\n", trans_brick->power.button, trans_brick->power.led_on, trans_brick->power.led_off);
	}

done:
	return status;
}

///////////////////////////////////////////////////////////////////////

// specific handlers

static
int make_primary(void *buf, struct mars_dent *dent)
{
	struct mars_dent *parent;
	struct mars_rotate *rot;
	int status = -EINVAL;

	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	rot = parent->d_private;
	CHECK_PTR(rot, done);

	rot->is_primary = (dent->new_link && !strcmp(dent->new_link, my_id()));
	status = 0;

done:
	return status;
}

static
int make_aio(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_brick *brick;
	struct aio_brick *_brick;
	int status = 0;

	if (!global->global_power.button || !dent->d_activate) {
		goto done;
	}
	if (mars_find_brick(global, &aio_brick_type, dent->d_path)) {
		goto done;
	}
	dent->d_kill_inactive = true;
	brick =
		make_brick_all(global,
			       dent,
			       10 * HZ,
			       dent->d_path,
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       dent->d_path,
			       (const char *[]){},
			       0);
	if (unlikely(!brick)) {
		status = -ENXIO;
		goto done;
	}
	brick->outputs[0]->output_name = dent->d_path;
	_brick = (void*)brick;
	_brick->outputs[0]->o_fdsync = true;
	status = mars_power_button((void*)brick, true);
	if (status < 0) {
		kill_all(buf, dent);
	}
 done:
	return status;
}

static int make_dev(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = parent->d_private;
	struct mars_brick *dev_brick;
	struct if_brick *_dev_brick;
	int status = 0;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		MARS_DBG("nothing to do\n");
		goto done;
	}

	status = make_log_finalize(global, dent->d_parent);
	if (status < 0) {
		MARS_DBG("logger not initialized\n");
		goto done;
	}
	if (!rot || !rot->is_primary) {
		MARS_DBG("I am not primary, don't show the device\n");
		goto done;
	}
	if (!rot->trans_brick || rot->trans_brick->do_replay || !rot->trans_brick->power.led_on || rot->trans_brick->power.led_off) {
		MARS_DBG("transaction logger not ready for writing\n");
		goto done;
	}

	status = _parse_args(dent, dent->new_link, 1);
	if (status < 0) {
		MARS_DBG("fail\n");
		goto done;
	}

	dev_brick =
		make_brick_all(global,
			       dent,
			       10 * HZ,
			       dent->d_argv[0],
			       (const struct generic_brick_type*)&if_brick_type,
			       (const struct generic_brick_type*[]){(const struct generic_brick_type*)&trans_logger_brick_type},
			       "%s/linuxdev-%s", 
			       (const char *[]){"%s/logger"},
			       1,
			       dent->d_parent->d_path,
			       dent->d_argv[0],
			       dent->d_parent->d_path);
	if (!dev_brick) {
		MARS_DBG("fail\n");
		return -1;
	}
	dev_brick->status_level = 1;
	_dev_brick = (void*)dev_brick;
#if 0
	if (_dev_brick->has_closed) {
		_dev_brick->has_closed = false;
		MARS_INF("rotating logfile for '%s'\n", parent->d_name);
		status = mars_power_button((void*)rot->trans_brick, false);
		rot->relevant_log = NULL;
	}
#endif

done:
	return status;
}

static int _make_direct(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_brick *brick;
	int status;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}
	status = _parse_args(dent, dent->new_link, 2);
	if (status < 0) {
		goto done;
	}
	brick = 
		make_brick_all(global,
			       dent,
			       10 * HZ,
			       dent->d_argv[0],
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       "%s/%s",
			       (const char *[]){},
			       0,
			       dent->d_parent->d_path,
			       dent->d_argv[0]);
	status = -1;
	if (!brick) {
		MARS_DBG("fail\n");
		goto done;
	}

	brick = 
		make_brick_all(global,
			       dent,
			       10 * HZ,
			       dent->d_argv[1],
			       (const struct generic_brick_type*)&if_brick_type,
			       (const struct generic_brick_type*[]){NULL},
			       "%s/linuxdev-%s",
			       (const char *[]){ "%s/%s" },
			       1,
			       dent->d_parent->d_path,
			       dent->d_argv[1],
			       dent->d_parent->d_path,
			       dent->d_argv[0]),
	status = -1;
	if (!brick) {
		MARS_DBG("fail\n");
		goto done;
	}

	status = 0;
done:
	MARS_DBG("status = %d\n", status);
	return status;
}

static int _make_copy(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	int status;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}
	status = _parse_args(dent, dent->new_link, 2);
	if (status < 0) {
		goto done;
	}

	status = __make_copy(global, dent, dent->d_path, dent->d_parent->d_path, (const char**)dent->d_argv, -1, NULL);

done:
	MARS_DBG("status = %d\n", status);
	return status;
}

static int make_sync(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	loff_t start_pos = 0;
	struct mars_dent *connect_dent;
	char *peer;
	struct copy_brick *copy = NULL;
	char *tmp = NULL;
	char *src = NULL;
	char *dst = NULL;
	int status;

	if (!global->global_power.button || !dent->d_activate || !dent->d_parent || !dent->new_link) {
		return 0;
	}

	/* Analyze replay position
	 */
	status = sscanf(dent->new_link, "%lld", &start_pos);
	if (status != 1) {
		MARS_ERR("bad syncstatus symlink syntax '%s' (%s)\n", dent->new_link, dent->d_path);
		status = -EINVAL;
		goto done;
	}
	dent->d_kill_inactive = true;

	/* Determine peer
	 */
	tmp = path_make("%s/connect-%s", dent->d_parent->d_path, my_id());
	status = -ENOMEM;
	if (unlikely(!tmp))
		goto done;
	connect_dent = (void*)mars_find_dent(global, tmp);
	if (!connect_dent || !connect_dent->new_link) {
		MARS_ERR("cannot determine peer, symlink '%s' is missing\n", tmp);
		status = -ENOENT;
		goto done;
	}
	connect_dent->d_kill_inactive = true;
	if (!connect_dent->d_activate) {
		MARS_DBG("sync paused: symlink '%s' is deactivated\n", tmp);
		status = 0;
		goto done;
	}
	peer = connect_dent->new_link;

	/* Start copy
	 */
	src = path_make("data-%s@%s", peer, peer);
	dst = path_make("data-%s", my_id());
	status = -ENOMEM;
	if (unlikely(!src || !dst))
		goto done;

	MARS_DBG("starting initial sync '%s' => '%s'\n", src, dst);

	{
		const char *argv[2] = { src, dst };
		status = __make_copy(global, dent, dent->d_path, dent->d_parent->d_path, argv, start_pos, &copy);
	}

	/* Update syncstatus symlink
	 */
	if (status >= 0 && copy && copy->power.button && copy->power.led_on) {
		kfree(src);
		kfree(dst);
		src = path_make("%lld", copy->copy_last);
		dst = path_make("%s/syncstatus-%s", dent->d_parent->d_path, my_id());
		status = -ENOMEM;
		if (unlikely(!src || !dst))
			goto done;
		status = mars_symlink(src, dst, NULL, 0);
		if (status >= 0 && copy->copy_last == copy->copy_end) {
			status = mars_power_button((void*)copy, false);
			MARS_DBG("copy switch status = %d\n", status);
		}
	}

done:
	MARS_DBG("status = %d\n", status);
	if (tmp)
		kfree(tmp);
	if (src)
		kfree(src);
	if (dst)
		kfree(dst);
	return status;
}

///////////////////////////////////////////////////////////////////////

// the order is important!
enum {
	// root element: this must have index 0
	CL_ROOT,
	// replacement for DNS in kernelspace
	CL_IPS,
	CL_PEERS,
	// resource definitions
	CL_RESOURCE,
	CL_CONNECT,
	CL_DATA,
	CL_PRIMARY,
	CL__FILE,
	CL_SYNC,
	CL__COPY,
	CL__REMOTE,
	CL__DIRECT,
	CL_REPLAYSTATUS,
	CL_LOG,
	CL_DEVICE,
};

/* Please keep the order the same as in the enum.
 */
static const struct light_class light_classes[] = {
	/* Placeholder for root node /mars/
	 */
	[CL_ROOT] = {
	},

	/* Directory containing the addresses of all peers
	 */
	[CL_IPS] = {
		.cl_name = "ips",
		.cl_len = 3,
		.cl_type = 'd',
		.cl_father = CL_ROOT,
#if 0
		.cl_forward = make_scan,
		.cl_backward = kill_scan,
#endif
	},
	/* Anyone participating in a MARS cluster must
	 * be named here (symlink pointing to the IP address).
	 * We have no DNS in kernel space.
	 */
	[CL_PEERS] = {
		.cl_name = "ip-",
		.cl_len = 3,
		.cl_type = 'l',
		.cl_father = CL_IPS,
#if 1
		.cl_forward = make_scan,
		.cl_backward = kill_scan,
#endif
	},

	/* Directory containing all items of a resource
	 */
	[CL_RESOURCE] = {
		.cl_name = "resource-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_father = CL_ROOT,
		.cl_forward = make_log_init,
		.cl_backward = NULL,
	},
	/* Symlink indicating the current peer
	 */
	[CL_CONNECT] = {
		.cl_name = "connect-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* File or symlink to the real device / real (sparse) file
	 * when hostcontext is missing, the corresponding peer will
	 * not participate in that resource.
	 */
	[CL_DATA] = {
		.cl_name = "data-",
		.cl_len = 5,
		.cl_type = 'F',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_aio,
		.cl_backward = kill_all,
	},
	/* Symlink pointing to the name of the primary node
	 */
	[CL_PRIMARY] = {
		.cl_name = "primary",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_primary,
		.cl_backward = NULL,
	},
	/* Only for testing: open local file
	 */
	[CL__FILE] = {
		.cl_name = "_file-",
		.cl_len = 6,
		.cl_type = 'F',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_aio,
		.cl_backward = kill_all,
	},
	/* symlink indicating the current status / end
	 * of initial data sync.
	 */
	[CL_SYNC] = {
		.cl_name = "syncstatus-",
		.cl_len = 11,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_sync,
		.cl_backward = kill_all,
	},
	/* Only for testing: make a copy instance
	 */
	[CL__COPY] = {
		.cl_name = "_copy-",
		.cl_len = 6,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = _make_copy,
		.cl_backward = kill_all,
	},
	/* Only for testing: access remote data directly
	 */
	[CL__REMOTE] = {
		.cl_name = "_remote-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = _make_remote,
		.cl_backward = _kill_remote,
	},
	/* Only for testing: access local data
	 */
	[CL__DIRECT] = {
		.cl_name = "_direct-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = _make_direct,
		.cl_backward = kill_all,
	},

	/* Passive symlink indicating the last state of
	 * transaction log replay.
	 */
	[CL_REPLAYSTATUS] = {
		.cl_name = "replay-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
#if 0
		.cl_forward = make_replay,
		.cl_backward = kill_all,
#endif
	},
	/* Logfiles for transaction logger
	 */
	[CL_LOG] = {
		.cl_name = "log-",
		.cl_len = 4,
		.cl_type = 'F',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
#if 1
		.cl_forward = make_log,
		.cl_backward = kill_all,
#endif
	},

	/* Name of the device appearing at the primary
	 */
	[CL_DEVICE] = {
		.cl_name = "device-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_dev,
		.cl_backward = kill_all,
	},
	{}
};

/* Helper routine to pre-determine the relevance of a name from the filesystem.
 */
static int light_checker(const char *path, const char *_name, int namlen, unsigned int d_type, int *prefix, int *serial)
{
	int class;
	int status = -2;
#ifdef MARS_DEBUGGING
	const char *name = kstrdup(_name, GFP_MARS);
	if (!name)
		return -ENOMEM;
#else
	const char *name = _name;
#endif

	//MARS_DBG("trying '%s' '%s'\n", path, name);
	for (class = CL_ROOT + 1; ; class++) {
		const struct light_class *test = &light_classes[class];
		int len = test->cl_len;
		if (!len || !test->cl_name)
			break;
		//MARS_DBG("   testing class '%s'\n", test->cl_name);
#if 0
		if (len != strlen(test->cl_name)) {
			MARS_ERR("internal table '%s': %d != %d\n", test->cl_name, len, (int)strlen(test->cl_name));
			len = strlen(test->cl_name);
		}
#endif
		if (namlen >= len && !memcmp(name, test->cl_name, len)) {
			//MARS_DBG("path '%s/%s' matches class %d '%s'\n", path, name, class, test->cl_name);
			// special contexts
			if (test->cl_serial) {
				int plus = 0;
				int count;
				count = sscanf(name+len, "%d%n", serial, &plus);
				if (count < 1) {
					//MARS_DBG("'%s' serial number mismatch at '%s'\n", name, name+len);
					status = -1;
					goto done;
				}
				len += plus;
				if (name[len] == '-')
					len++;
			}
			*prefix = len;
			if (test->cl_hostcontext) {
				if (memcmp(name+len, my_id(), namlen-len)) {
					//MARS_DBG("context mismatch '%s' at '%s'\n", name, name+len);
					status = -1;
					goto done;
				}
			}
			status = class;
			goto done;
		}
	}
	//MARS_DBG("no match for '%s' '%s'\n", path, name);
done:
#ifdef MARS_DEBUGGING
	if (name)
		kfree(name);
#endif
	return status;
}

/* Do some syntactic checks, then delegate work to the real worker functions
 * from the light_classes[] table.
 */
static int light_worker(struct mars_global *global, struct mars_dent *dent, bool direction)
{
	light_worker_fn worker;
	int class = dent->d_class;

	if (class < 0 || class >= sizeof(light_classes)/sizeof(struct light_class)) {
		MARS_ERR_ONCE(dent, "bad internal class %d of '%s'\n", class, dent->d_path);
		return -EINVAL;
	}
	switch (light_classes[class].cl_type) {
	case 'd':
		if (!S_ISDIR(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a directory, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'f':
		if (!S_ISREG(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a regular file, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'F':
		if (!S_ISREG(dent->new_stat.mode) && !S_ISLNK(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a regular file or a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'l':
		if (!S_ISLNK(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	}
	if (likely(class > CL_ROOT)) {
		int father = light_classes[class].cl_father;
		if (father == CL_ROOT) {
			if (unlikely(dent->d_parent)) {
				MARS_ERR_ONCE(dent, "'%s' is not at the root of the hierarchy\n", dent->d_path);
				return -EINVAL;
			}
		} else if (unlikely(!dent->d_parent || dent->d_parent->d_class != father)) {
			MARS_ERR_ONCE(dent, "last component '%s' from '%s' is at the wrong position in the hierarchy (class = %d, parent_class = %d, parent = '%s')\n", dent->d_name, dent->d_path, father, dent->d_parent ? dent->d_parent->d_class : -9999, dent->d_parent ? dent->d_parent->d_path : "");
			return -EINVAL;
		}
	}
	if (direction) {
		worker = light_classes[class].cl_backward;
	} else {
		worker = light_classes[class].cl_forward;
	}
	if (worker) {
		int status;
		//MARS_DBG("working %s on '%s' rest='%s'\n", direction ? "backward" : "forward", dent->d_path, dent->d_rest);
		status = worker(global, (void*)dent);
		MARS_DBG("worked %s on '%s', status = %d\n", direction ? "backward" : "forward", dent->d_path, status);
		return status;
	}
	return 0;
}

void _show_status(struct mars_global *global)
{
	struct list_head *tmp;
	
	down(&global->mutex);
	for (tmp = global->brick_anchor.next; tmp != &global->brick_anchor; tmp = tmp->next) {
		struct mars_brick *test;
		const char *path;
		char *src;
		char *dst;
		int status;
		
		test = container_of(tmp, struct mars_brick, global_brick_link);
		if (test->status_level <= 0)
			continue;
		
		path = test->brick_path;
		if (!path) {
			MARS_DBG("bad path\n");
			continue;
		}
		if (*path != '/') {
			MARS_DBG("bogus path '%s'\n", path);
			continue;
		}

		src = test->power.led_on ? "1" : "0";
		dst = _backskip_replace(path, '/', true, "/actual-%s.", my_id());
		if (!dst)
			continue;

		status = mars_symlink(src, dst, NULL, 0);
		MARS_DBG("status symlink '%s' -> '%s' status = %d\n", dst, src, status);
		if (test->status_level > 1) {
			char perc[8];
			char *dst2 = path_make("%s.percent", dst);
			if (likely(dst2)) {
				snprintf(perc, sizeof(perc), "%d", test->power.percent_done);
				status = mars_symlink(perc, dst2, NULL, 0);
				MARS_DBG("percent symlink '%s' -> '%s' status = %d\n", dst2, src, status);
				kfree(dst2);
			}
		}
		kfree(dst);
	}
	up(&global->mutex);
}

#ifdef STAT_DEBUGGING
static
void _show_statist(struct mars_global *global)
{
	struct list_head *tmp;
	int dent_count = 0;
	int brick_count = 0;
	
	down(&global->mutex);
	MARS_STAT("----------- lists:\n");
	for (tmp = global->dent_anchor.next; tmp != &global->dent_anchor; tmp = tmp->next) {
		struct mars_dent *dent;
		dent = container_of(tmp, struct mars_dent, dent_link);
		MARS_STAT("dent '%s'\n", dent->d_path);
		dent_count++;
	}
	for (tmp = global->brick_anchor.next; tmp != &global->brick_anchor; tmp = tmp->next) {
		struct mars_brick *test;
		int i;
		test = container_of(tmp, struct mars_brick, global_brick_link);
		MARS_STAT("brick type = %s path = '%s' name = '%s' level = %d button = %d off = %d on = %d\n", test->type->type_name, test->brick_path, test->brick_name, test->status_level, test->power.button, test->power.led_off, test->power.led_on);
		brick_count++;
		for (i = 0; i < test->nr_inputs; i++) {
			struct mars_input *input = test->inputs[i];
			struct mars_output *output = input ? input->connect : NULL;
			if (output) {
				MARS_STAT("  input %d connected with %s path = '%s' name = '%s'\n", i, output->brick->type->type_name, output->brick->brick_path, output->brick->brick_name);
			} else {
				MARS_STAT("  input %d not connected\n", i);
			}
		}
	}
	up(&global->mutex);
	
	MARS_INF("----------- STATISTICS: %d dents, %d bricks\n", dent_count, brick_count);
}
#endif

static int light_thread(void *data)
{
	char *id = my_id();
	int status = 0;
	struct mars_global global = {
		.dent_anchor = LIST_HEAD_INIT(global.dent_anchor),
		.brick_anchor = LIST_HEAD_INIT(global.brick_anchor),
		.global_power = {
			.button = true,
		},
		.mutex = __SEMAPHORE_INITIALIZER(global.mutex, 1),
		.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(global.main_event),
	};
	mars_global = &global; // TODO: cleanup, avoid stack

	if (!id || strlen(id) < 2) {
		MARS_ERR("invalid hostname\n");
		status = -EFAULT;
		goto done;
	}	

	//fake_mm();

	MARS_INF("-------- starting as host '%s' ----------\n", id);

        while (global.global_power.button || !list_empty(&global.brick_anchor)) {
		int status;
		global.global_power.button = !kthread_should_stop();

		status = mars_dent_work(&global, "/mars", sizeof(struct mars_dent), light_checker, light_worker, &global, 3);
		MARS_DBG("worker status = %d\n", status);

		_show_status(&global);
#ifdef STAT_DEBUGGING
		_show_statist(&global);
#endif

		msleep(1000);

		wait_event_interruptible_timeout(global.main_event, global.main_trigger, 30 * HZ);
		global.main_trigger = false;
	}

done:
	MARS_INF("-------- cleaning up ----------\n");

	mars_free_dent_all(&global.dent_anchor);

	mars_global = NULL;
	main_thread = NULL;

	MARS_INF("-------- done status = %d ----------\n", status);
	//cleanup_mm();
	return status;
}

static void __exit exit_light(void)
{
	// TODO: make this thread-safe.
	struct task_struct *thread = main_thread;
	if (thread) {
		main_thread = NULL;
		MARS_DBG("====================== stopping everything...\n");
		kthread_stop_nowait(thread);
		mars_trigger();
		kthread_stop(thread);
		put_task_struct(thread);
		MARS_DBG("====================== stopped everything.\n");
	}
}

static int __init init_light(void)
{
	struct task_struct *thread;
	thread = kthread_create(light_thread, NULL, "mars_light");
	if (IS_ERR(thread)) {
		return PTR_ERR(thread);
	}
	get_task_struct(thread);
	main_thread = thread;
	wake_up_process(thread);
#if 1 // quirk: bump the memory reserve limits. TODO: determine right values.
	{
		extern int min_free_kbytes;
		min_free_kbytes *= 4;
		setup_per_zone_wmarks();
	}
#endif
	return 0;
}

// force module loading
const void *dummy1 = &client_brick_type;
const void *dummy2 = &server_brick_type;

MODULE_DESCRIPTION("MARS Light");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_light);
module_exit(exit_light);