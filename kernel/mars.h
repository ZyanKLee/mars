// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_H
#define MARS_H

#include <linux/semaphore.h>
#include <linux/rwsem.h>

// check the Kconfig environment

#ifndef CONFIG_MARS_MODULE
// when unsure, include faked config file
#include "mars_config.h"
#endif

#ifndef CONFIG_64BIT
#error MARS is only tested under 64bit
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

#define OBJ_TYPE_MREF			0
#define OBJ_TYPE_MAX			1

#include "brick.h"
#include "brick_mem.h"
#include "lamport.h"
#include "lib_timing.h"

/////////////////////////////////////////////////////////////////////////

// MARS-specific debugging helpers

#define _MARS_MSG(_class, _dump, _fmt, _args...)			\
	brick_say(_class, _dump, "MARS", __BASE_FILE__, __LINE__, __FUNCTION__, _fmt, ##_args)

#define MARS_FAT(_fmt, _args...) _MARS_MSG(SAY_FATAL, true,  _fmt, ##_args)
#define MARS_ERR(_fmt, _args...) _MARS_MSG(SAY_ERROR, true,  _fmt, ##_args)
#define MARS_WRN(_fmt, _args...) _MARS_MSG(SAY_WARN,  false, _fmt, ##_args)
#define MARS_INF(_fmt, _args...) _MARS_MSG(SAY_INFO,  false, _fmt, ##_args)

#ifdef MARS_DEBUGGING
#define MARS_DBG(_fmt, _args...) _MARS_MSG(SAY_DEBUG, false, _fmt, ##_args)
#else
#define MARS_DBG(_args...) /**/
#endif

/////////////////////////////////////////////////////////////////////////

// MARS-specific definitions

#define MARS_PRIO_HIGH			-1
#define MARS_PRIO_NORMAL		0 // this is automatically used by memset()
#define MARS_PRIO_LOW			1
#define MARS_PRIO_NR			3

// object stuff

/* mref flags */

/* Don't change the order, only add new flags at the end or at
 * pre-existing gaps.
 */
enum {
	__MREF_UPTODATE,
	__MREF_READING,
	__MREF_WRITING,
	// semantics which _must_ be obeyed
	__MREF_FLUSH = 16,	 // force total ordering
	// semantics which _may_ be expoited for better performance
	__MREF_PERF_NOMETA = 24, // allow performance improvements for bulk data
	__MREF_PERF_NOSYNC,	 // allow skipping write-through
};

#define MREF_UPTODATE			(1UL << __MREF_UPTODATE)
#define MREF_READING			(1UL << __MREF_READING)
#define MREF_WRITING			(1UL << __MREF_WRITING)
#define MREF_FLUSH			(1UL << __MREF_FLUSH)
#define MREF_PERF_NOMETA		(1UL << __MREF_PERF_NOMETA)
#define MREF_PERF_NOSYNC		(1UL << __MREF_PERF_NOSYNC)

extern const struct generic_object_type mref_type;

#define MARS_CHECKSUM_SIZE		16

#define MREF_OBJECT(OBJTYPE)						\
	CALLBACK_OBJECT(OBJTYPE);					\
	/* supplied by caller */					\
	void  *ref_data;	 /* preset to NULL for buffered IO */	\
	loff_t ref_pos; 						\
	int    ref_len; 						\
	int    ref_may_write;						\
	int    ref_prio;						\
	int    ref_timeout;						\
	int    ref_cs_mode; /* 0 = off, 1 = checksum + data, 2 = checksum only */\
	/* maintained by the ref implementation, readable for callers */\
	loff_t ref_total_size; /* just for info, need not be implemented */\
	unsigned char ref_checksum[MARS_CHECKSUM_SIZE]; 		\
	int    ref_flags;						\
	int    ref_rw;							\
	int    ref_id; /* not mandatory; may be used for identification */\

struct mref_object {
	MREF_OBJECT(mref);
};

// internal helper structs

struct mars_info {
	loff_t current_size;
	int tf_align;	 // transfer alignment constraint
	int tf_min_size; // transfer is only possible in multiples of this
};

// brick stuff

#define MARS_BRICK(BRITYPE)						\
	GENERIC_BRICK(BRITYPE); 					\
	struct generic_object_layout mref_object_layout;		\
	struct list_head global_brick_link;				\
	struct list_head dent_brick_link;				\
	const char *brick_name; 					\
	const char *brick_path; 					\
	struct mars_global *global;					\
	int kill_round; 						\
	bool killme;							\
	void (*show_status)(struct mars_brick *brick, bool shutdown);	\

struct mars_brick {
	MARS_BRICK(mars);
};

#define MARS_INPUT(BRITYPE)						\
	GENERIC_INPUT(BRITYPE); 					\

struct mars_input {
	MARS_INPUT(mars);
};

#define MARS_OUTPUT(BRITYPE)						\
	GENERIC_OUTPUT(BRITYPE);					\

struct mars_output {
	MARS_OUTPUT(mars);
};

#define MARS_BRICK_OPS(BRITYPE) 					\
	GENERIC_BRICK_OPS(BRITYPE);					\
	char *(*brick_statistics)(struct BRITYPE##_brick *brick, int verbose);\
	void (*reset_statistics)(struct BRITYPE##_brick *brick);	\

#define MARS_OUTPUT_OPS(BRITYPE)					\
	GENERIC_OUTPUT_OPS(BRITYPE);					\
	int  (*mars_get_info)(struct BRITYPE##_output *output, struct mars_info *info);\
	/* mref */							\
	int  (*mref_get)(struct BRITYPE##_output *output, struct mref_object *mref);\
	void (*mref_io)(struct BRITYPE##_output *output, struct mref_object *mref);\
	void (*mref_put)(struct BRITYPE##_output *output, struct mref_object *mref);\

// all non-extendable types

#define _MARS_TYPES(BRITYPE)						\
									\
struct BRITYPE##_brick_ops {						\
	MARS_BRICK_OPS(BRITYPE);					\
};									\
									\
struct BRITYPE##_output_ops {						\
	MARS_OUTPUT_OPS(BRITYPE);					\
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


#define MARS_TYPES(BRITYPE)						\
									\
_MARS_TYPES(BRITYPE)							\
									\
DECLARE_ASPECT_FUNCTIONS(BRITYPE,mref); 				\
extern int init_mars_##BRITYPE(void);					\
extern void exit_mars_##BRITYPE(void);


// instantiate pseudo base-classes

DECLARE_OBJECT_FUNCTIONS(mref);
_MARS_TYPES(mars);
DECLARE_ASPECT_FUNCTIONS(mars,mref);

/////////////////////////////////////////////////////////////////////////

// MARS-specific helpers

#define MARS_MAKE_STATICS(BRITYPE)					\
									\
int BRITYPE##_brick_nr = -EEXIST;					\
EXPORT_SYMBOL_GPL(BRITYPE##_brick_nr);					\
									\
static const struct generic_aspect_type BRITYPE##_mref_aspect_type = {	\
	.aspect_type_name = #BRITYPE "_mref_aspect_type",		\
	.object_type = &mref_type,					\
	.aspect_size = sizeof(struct BRITYPE##_mref_aspect),		\
	.init_fn = BRITYPE##_mref_aspect_init_fn,			\
	.exit_fn = BRITYPE##_mref_aspect_exit_fn,			\
};									\
									\
static const struct generic_aspect_type *BRITYPE##_aspect_types[OBJ_TYPE_MAX] = {\
	[OBJ_TYPE_MREF] = &BRITYPE##_mref_aspect_type,			\
};									\

extern const struct meta mars_info_meta[];
extern const struct meta mars_mref_meta[];
extern const struct meta mars_timespec_meta[];

/////////////////////////////////////////////////////////////////////////

/* Some minimal upcalls from generic IO layer to the strategy layer.
 * TODO: abstract away.
 */

extern void mars_power_led_on(struct mars_brick *brick, bool val);
extern void mars_power_led_off(struct mars_brick *brick, bool val);

/* this should disappear!
 */
extern void (*_mars_trigger)(void);
extern void (*_mars_remote_trigger)(void);
#define mars_trigger() do { if (_mars_trigger) { MARS_DBG("trigger...\n"); _mars_trigger(); } } while (0)
#define mars_remote_trigger() do { if (_mars_remote_trigger) { MARS_DBG("remote_trigger...\n"); _mars_remote_trigger(); } } while (0)

/////////////////////////////////////////////////////////////////////////

/* Some global stuff.
 */

extern struct banning mars_global_ban;

extern atomic_t mars_global_io_flying;

extern int mars_throttle_start;
extern int mars_throttle_end;

/////////////////////////////////////////////////////////////////////////

/* Some special brick types for avoidance of cyclic references.
 *
 * The client/server network bricks use this for independent instantiation
 * from the main instantiation logic (separate modprobe for mars_server
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
		MARS_DBG("initialized fake\n");
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
			MARS_ERR("cannot cleanup fake, remain = %d\n", remain);
		} else {
			MARS_DBG("cleaning up fake\n");
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
		MARS_DBG("using fake, count=%d\n", atomic_read(&mm_fake_count));
		use_mm(mm_fake);
	}
}

/* Cleanup faked mm, otherwise do_exit() will crash
 */
static inline void unuse_fake_mm(void)
{
	if (current->mm == mm_fake && mm_fake) {
		MARS_DBG("unusing fake, count=%d\n", atomic_read(&mm_fake_count));
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

extern int mars_digest_size;
extern void mars_digest(unsigned char *digest, void *data, int len);
extern void mref_checksum(struct mref_object *mref);

/////////////////////////////////////////////////////////////////////////

// init

extern int init_mars(void);
extern void exit_mars(void);

#endif
