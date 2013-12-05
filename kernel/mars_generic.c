// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/utsname.h>

#include "mars.h"
#include "mars_client.h"

//////////////////////////////////////////////////////////////

// infrastructure

struct banning mars_global_ban = {};
EXPORT_SYMBOL_GPL(mars_global_ban);
atomic_t mars_global_io_flying = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(mars_global_io_flying);

static char *id = NULL;

/* TODO: better use MAC addresses (or motherboard IDs where available).
 * Or, at least, some checks for MAC addresses should be recorded / added.
 * When the nodename is misconfigured, data might be scrambled.
 * MAC addresses should be more secure.
 * In ideal case, further checks should be added to prohibit accidental
 * name clashes.
 */
char *my_id(void)
{
	struct new_utsname *u;
	if (!id) {
		//down_read(&uts_sem); // FIXME: this is currenty not EXPORTed from the kernel!
		u = utsname();
		if (u) {
			id = brick_strdup(u->nodename);
		}
		//up_read(&uts_sem);
	}
	return id;
}
EXPORT_SYMBOL_GPL(my_id);

//////////////////////////////////////////////////////////////

// object stuff

const struct generic_object_type aio_type = {
	.object_type_name = "aio",
	.default_size = sizeof(struct aio_object),
	.object_type_nr = OBJ_TYPE_AIO,
};
EXPORT_SYMBOL_GPL(aio_type);

//////////////////////////////////////////////////////////////

// brick stuff

/////////////////////////////////////////////////////////////////////

// meta descriptions

const struct meta mars_info_meta[] = {
	META_INI(current_size,	  struct mars_info, FIELD_INT),
	META_INI(tf_align,	  struct mars_info, FIELD_INT),
	META_INI(tf_min_size,	  struct mars_info, FIELD_INT),
	{}
};
EXPORT_SYMBOL_GPL(mars_info_meta);

const struct meta mars_aio_meta[] = {
	META_INI(_object_cb.cb_error, struct aio_object, FIELD_INT),
	META_INI(io_pos,	   struct aio_object, FIELD_INT),
	META_INI(io_len,	   struct aio_object, FIELD_INT),
	META_INI(io_may_write,	  struct aio_object, FIELD_INT),
	META_INI(io_prio,	   struct aio_object, FIELD_INT),
	META_INI(io_cs_mode,	   struct aio_object, FIELD_INT),
	META_INI(io_timeout,	   struct aio_object, FIELD_INT),
	META_INI(io_total_size,   struct aio_object, FIELD_INT),
	META_INI(io_checksum,	   struct aio_object, FIELD_RAW),
	META_INI(io_flags,	   struct aio_object, FIELD_INT),
	META_INI(io_rw,    struct aio_object, FIELD_INT),
	META_INI(io_id,    struct aio_object, FIELD_INT),
	{}
};
EXPORT_SYMBOL_GPL(mars_aio_meta);

const struct meta mars_timespec_meta[] = {
	META_INI_TRANSFER(tv_sec,  struct timespec, FIELD_UINT, 8),
	META_INI_TRANSFER(tv_nsec, struct timespec, FIELD_UINT, 4),
	{}
};
EXPORT_SYMBOL_GPL(mars_timespec_meta);


//////////////////////////////////////////////////////////////

// crypto stuff

#include <linux/crypto.h>

static struct crypto_hash *mars_tfm = NULL;
static struct semaphore tfm_sem = __SEMAPHORE_INITIALIZER(tfm_sem, 1);
int mars_digest_size = 0;
EXPORT_SYMBOL_GPL(mars_digest_size);

void mars_digest(unsigned char *digest, void *data, int len)
{
	struct hash_desc desc = {
		.tfm = mars_tfm,
		.flags = 0,
	};
	struct scatterlist sg;

	memset(digest, 0, mars_digest_size);

	// TODO: use per-thread instance, omit locking
	down(&tfm_sem);

	crypto_hash_init(&desc);
	sg_init_table(&sg, 1);
	sg_set_buf(&sg, data, len);
	crypto_hash_update(&desc, &sg, sg.length);
	crypto_hash_final(&desc, digest);
	up(&tfm_sem);
}
EXPORT_SYMBOL_GPL(mars_digest);

void aio_checksum(struct aio_object *aio)
{
	unsigned char checksum[mars_digest_size];
	int len;

	if (aio->io_cs_mode <= 0 || !aio->io_data)
		return;

	mars_digest(checksum, aio->io_data, aio->io_len);

	len = sizeof(aio->io_checksum);
	if (len > mars_digest_size)
		len = mars_digest_size;
	memcpy(&aio->io_checksum, checksum, len);
}
EXPORT_SYMBOL_GPL(aio_checksum);

/////////////////////////////////////////////////////////////////////

// init stuff

void (*_mars_trigger)(void) = NULL;
EXPORT_SYMBOL_GPL(_mars_trigger);

struct mm_struct *mm_fake = NULL;
EXPORT_SYMBOL_GPL(mm_fake);
struct task_struct *mm_fake_task = NULL;
atomic_t mm_fake_count = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(mm_fake_count);

int __init init_mars(void)
{
	MARS_INF("init_mars()\n");

	set_fake();

	mars_tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (!mars_tfm) {
		MARS_ERR("cannot alloc crypto hash\n");
		return -ENOMEM;
	}
	if (IS_ERR(mars_tfm)) {
		MARS_ERR("alloc crypto hash failed, status = %d\n", (int)PTR_ERR(mars_tfm));
		return PTR_ERR(mars_tfm);
	}
	mars_digest_size = crypto_hash_digestsize(mars_tfm);
	MARS_INF("digest_size = %d\n", mars_digest_size);

	return 0;
}

void __exit exit_mars(void)
{
	MARS_INF("exit_mars()\n");

	put_fake();

	if (mars_tfm) {
		crypto_free_hash(mars_tfm);
	}

	if (id) {
		brick_string_free(id);
		id = NULL;
	}
}
