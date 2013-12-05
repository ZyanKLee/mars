// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef XIO_H
#define XIO_H

#include <linux/semaphore.h>
#include <linux/rwsem.h>

// check the Kconfig environment

#ifndef CONFIG_MARS_MODULE
// when unsure, include faked config file
#include "mars_config.h"
#endif

#ifndef CONFIG_64BIT
#error XIO is only tested under 64bit
#endif
#ifndef CONFIG_BLOCK
#error CONFIG_BLOCK must be set
#endif
#ifndef CONFIG_PROC_SYSCTL
#error CONFIG_PROC_SYSCTL must be set
#endif
#ifndef CONFIG_HIGH_RES_TIMERS
#error CONFIG_HIGH_RES_TIMERS must be set
#endif

/////////////////////////////////////////////////////////////////////////

// include the generic brick infrastructure

#define OBJ_TYPE_AIO			0
#define OBJ_TYPE_MAX			1

#include "../brick.h"
#include "../brick_mem.h"
#include "../lamport.h"
#include "../lib_timing.h"

/////////////////////////////////////////////////////////////////////////

// XIO-specific debugging helpers

#define _XIO_MSG(_class, _dump, _fmt, _args...) 			\
	brick_say(_class, _dump, "XIO", __BASE_FILE__, __LINE__, __FUNCTION__, _fmt, ##_args)

#define XIO_FAT(_fmt, _args...) _XIO_MSG(SAY_FATAL, true,  _fmt, ##_args)
#define XIO_ERR(_fmt, _args...) _XIO_MSG(SAY_ERROR, true,  _fmt, ##_args)
#define XIO_WRN(_fmt, _args...) _XIO_MSG(SAY_WARN,  false, _fmt, ##_args)
#define XIO_INF(_fmt, _args...) _XIO_MSG(SAY_INFO,  false, _fmt, ##_args)

#ifdef XIO_DEBUGGING
#define XIO_DBG(_fmt, _args...) _XIO_MSG(SAY_DEBUG, false, _fmt, ##_args)
#else
#define XIO_DBG(_args...) /**/
#endif

/////////////////////////////////////////////////////////////////////////

// XIO-specific definitions

#define XIO_PRIO_HIGH			-1
#define XIO_PRIO_NORMAL 		0 // this is automatically used by memset()
#define XIO_PRIO_LOW			1
#define XIO_PRIO_NR			3

// object stuff

/* aio flags */

/* Don't change the order, only add new flags at the end or at
 * pre-existing gaps.
 */
enum {
	__AIO_UPTODATE,
	__AIO_READING,
	__AIO_WRITING,
	// semantics which _must_ be obeyed
	__AIO_FLUSH = 16,	 // force total ordering
	// semantics which _may_ be expoited for better performance
	__AIO_PERF_NOMETA = 24, // allow performance improvements for bulk data
	__AIO_PERF_NOSYNC,	 // allow skipping write-through
};

#define AIO_UPTODATE			(1UL << __AIO_UPTODATE)
#define AIO_READING			(1UL << __AIO_READING)
#define AIO_WRITING			(1UL << __AIO_WRITING)
#define AIO_FLUSH			(1UL << __AIO_FLUSH)
#define AIO_PERF_NOMETA 		(1UL << __AIO_PERF_NOMETA)
#define AIO_PERF_NOSYNC 		(1UL << __AIO_PERF_NOSYNC)

extern const struct generic_object_type aio_type;

#define MARS_CHECKSUM_SIZE		16

#define AIO_OBJECT(OBJTYPE)						\
	CALLBACK_OBJECT(OBJTYPE);					\
	/* supplied by caller */					\
	void  *io_data;  /* preset to NULL for buffered IO */		\
	loff_t io_pos;							\
	int    io_len;							\
	int    io_may_write;						\
	int    io_prio; 						\
	int    io_timeout;						\
	int    io_cs_mode; /* 0 = off, 1 = checksum + data, 2 = checksum only */\
	/* maintained by the aio implementation, readable for callers */\
	loff_t io_total_size; /* just for info, need not be implemented */\
	unsigned char io_checksum[MARS_CHECKSUM_SIZE];			\
	int    io_flags;						\
	int    io_rw;							\
	int    io_id; /* not mandatory; may be used for identification */\

struct aio_object {
	AIO_OBJECT(aio);
};

// internal helper structs

struct xio_info {
	loff_t current_size;
	int tf_align;	 // transfer alignment constraint
	int tf_min_size; // transfer is only possible in multiples of this
};

// brick stuff

#define XIO_BRICK(BRITYPE)						\
	GENERIC_BRICK(BRITYPE); 					\
	struct generic_object_layout aio_object_layout; 		\
	struct list_head global_brick_link;				\
	struct list_head dent_brick_link;				\
	const char *brick_name; 					\
	const char *brick_path; 					\
	struct xio_global *global;					\
	int kill_round; 						\
	bool killme;							\
	void (*show_status)(struct xio_brick *brick, bool shutdown);	\

struct xio_brick {
	XIO_BRICK(xio);
};

#define XIO_INPUT(BRITYPE)						\
	GENERIC_INPUT(BRITYPE); 					\

struct xio_input {
	XIO_INPUT(xio);
};

#define XIO_OUTPUT(BRITYPE)						\
	GENERIC_OUTPUT(BRITYPE);					\

struct xio_output {
	XIO_OUTPUT(xio);
};

#define XIO_BRICK_OPS(BRITYPE)						\
	GENERIC_BRICK_OPS(BRITYPE);					\
	char *(*brick_statistics)(struct BRITYPE##_brick *brick, int verbose);\
	void (*reset_statistics)(struct BRITYPE##_brick *brick);	\

#define XIO_OUTPUT_OPS(BRITYPE) 					\
	GENERIC_OUTPUT_OPS(BRITYPE);					\
	int  (*xio_get_info)(struct BRITYPE##_output *output, struct xio_info *info);\
	/* aio */							\
	int  (*aio_get)(struct BRITYPE##_output *output, struct aio_object *aio);\
	void (*aio_io)(struct BRITYPE##_output *output, struct aio_object *aio);\
	void (*aio_put)(struct BRITYPE##_output *output, struct aio_object *aio);\

// all non-extendable types

#define _XIO_TYPES(BRITYPE)						\
									\
struct BRITYPE##_brick_ops {						\
	XIO_BRICK_OPS(BRITYPE); 					\
};									\
									\
struct BRITYPE##_output_ops {						\
	XIO_OUTPUT_OPS(BRITYPE);					\
};									\
									\
struct BRITYPE##_brick_type {						\
	GENERIC_BRICK_TYPE(BRITYPE);					\
};									\
									\
struct BRITYPE##_input_type {						\
	GENERIC_INPUT_TYPE(BRITYPE);					\
};									\
									\
struct BRITYPE##_output_type {						\
	GENERIC_OUTPUT_TYPE(BRITYPE);					\
};									\
									\
struct BRITYPE##_callback {						\
	GENERIC_CALLBACK(BRITYPE);					\
};									\
									\
DECLARE_BRICK_FUNCTIONS(BRITYPE);					\


#define XIO_TYPES(BRITYPE)						\
									\
_XIO_TYPES(BRITYPE)							\
									\
DECLARE_ASPECT_FUNCTIONS(BRITYPE,aio);					\
extern int init_xio_##BRITYPE(void);					\
extern void exit_xio_##BRITYPE(void);


// instantiate pseudo base-classes

DECLARE_OBJECT_FUNCTIONS(aio);
_XIO_TYPES(xio);
DECLARE_ASPECT_FUNCTIONS(xio,aio);

/////////////////////////////////////////////////////////////////////////

// XIO-specific helpers

#define XIO_MAKE_STATICS(BRITYPE)					\
									\
int BRITYPE##_brick_nr = -EEXIST;					\
EXPORT_SYMBOL_GPL(BRITYPE##_brick_nr);					\
									\
static const struct generic_aspect_type BRITYPE##_aio_aspect_type = {	\
	.aspect_type_name = #BRITYPE "_aio_aspect_type",		\
	.object_type = &aio_type,					\
	.aspect_size = sizeof(struct BRITYPE##_aio_aspect),		\
	.init_fn = BRITYPE##_aio_aspect_init_fn,			\
	.exit_fn = BRITYPE##_aio_aspect_exit_fn,			\
};									\
									\
static const struct generic_aspect_type *BRITYPE##_aspect_types[OBJ_TYPE_MAX] = {\
	[OBJ_TYPE_AIO] = &BRITYPE##_aio_aspect_type,			\
};									\

extern const struct meta xio_info_meta[];
extern const struct meta xio_aio_meta[];
extern const struct meta xio_timespec_meta[];

/////////////////////////////////////////////////////////////////////////

/* Some minimal upcalls from generic IO layer to the strategy layer.
 * TODO: abstract away.
 */

extern void xio_set_power_led_on(struct xio_brick *brick, bool val);
extern void xio_set_power_led_off(struct xio_brick *brick, bool val);

/* this should disappear!
 */
extern void (*_local_trigger)(void);
extern void (*_remote_trigger)(void);
#define local_trigger() do { if (_local_trigger) { XIO_DBG("trigger...\n"); _local_trigger(); } } while (0)
#define remote_trigger() do { if (_remote_trigger) { XIO_DBG("remote_trigger...\n"); _remote_trigger(); } } while (0)

/////////////////////////////////////////////////////////////////////////

/* Some global stuff.
 */

extern struct banning xio_global_ban;

extern atomic_t xio_global_io_flying;

extern int xio_throttle_start;
extern int xio_throttle_end;

/////////////////////////////////////////////////////////////////////////

/* Some special brick types for avoidance of cyclic references.
 *
 * The client/server network bricks use this for independent instantiation
 * from the main instantiation logic (separate modprobe for xio_server
 * is possible).
 */
extern const struct generic_brick_type *_client_brick_type;
extern const struct generic_brick_type *_bio_brick_type;
extern const struct generic_brick_type *_aio_brick_type;
extern const struct generic_brick_type *_sio_brick_type;

#ifndef CONFIG_MARS_PREFER_SIO

/* Kludge: our kernel threads will have no mm context, but need one
 * for stuff like ioctx_alloc() / aio_setup_ring() etc
 * which expect userspace resources.
 * We fake one.
 * TODO: factor out the userspace stuff from AIO such that
 * this fake is no longer necessary.
 * Even better: replace do_mmap() in AIO stuff by something
 * more friendly to kernelspace apps.
 */
#include <linux/mmu_context.h>

extern struct mm_struct *mm_fake;
extern struct task_struct *mm_fake_task;
extern atomic_t mm_fake_count;

static inline void set_fake(void)
{
	mm_fake = current->mm;
	if (mm_fake) {
		XIO_DBG("initialized fake\n");
		mm_fake_task = current;
		get_task_struct(current); // paired with put_task_struct()
		atomic_inc(&mm_fake->mm_count); // paired with mmdrop()
		atomic_inc(&mm_fake->mm_users); // paired with mmput()
	}
}

static inline void put_fake(void)
{
	if (mm_fake && mm_fake_task) {
		int remain = atomic_read(&mm_fake_count);
		if (unlikely(remain != 0)) {
			XIO_ERR("cannot cleanup fake, remain = %d\n", remain);
		} else {
			XIO_DBG("cleaning up fake\n");
			mmput(mm_fake);
			mmdrop(mm_fake);
			mm_fake = NULL;
			put_task_struct(mm_fake_task);
			mm_fake_task = NULL;
		}
	}
}

static inline void use_fake_mm(void)
{
	if (!current->mm && mm_fake) {
		atomic_inc(&mm_fake_count);
		XIO_DBG("using fake, count=%d\n", atomic_read(&mm_fake_count));
		use_mm(mm_fake);
	}
}

/* Cleanup faked mm, otherwise do_exit() will crash
 */
static inline void unuse_fake_mm(void)
{
	if (current->mm == mm_fake && mm_fake) {
		XIO_DBG("unusing fake, count=%d\n", atomic_read(&mm_fake_count));
		atomic_dec(&mm_fake_count);
		unuse_mm(mm_fake);
		current->mm = NULL;
	}
}

#else
static inline void set_fake(void) {}
static inline void put_fake(void) {}
static inline void use_fake_mm(void) {}
static inline void unuse_fake_mm(void) {}
#endif

/////////////////////////////////////////////////////////////////////////

/* Crypto stuff
 */

extern int xio_digest_size;
extern void xio_digest(unsigned char *digest, void *data, int len);
extern void aio_checksum(struct aio_object *aio);

/////////////////////////////////////////////////////////////////////////

// init

extern int init_xio(void);
extern void exit_xio(void);

#endif
