// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef BRICK_H
#define BRICK_H

#include <linux/spinlock.h>
#include <linux/sched.h>

#ifdef _STRATEGY
#define _STRATEGY_CODE(X) X
#define _NORMAL_CODE(X) /**/
#else
#define _STRATEGY_CODE(X) /**/
#define _NORMAL_CODE(X) X
#endif

#define BRICK_ERROR "BRICK_ERROR " __BASE_FILE__ ": "
#define BRICK_INFO  "BRICK_INFO  " __BASE_FILE__ ": "
#define BRICK_DEBUG "BRICK_DEBUG " __BASE_FILE__ ": "

#define BRICK_ERR(fmt, args...) printk(BRICK_ERROR "%s(): " fmt, __FUNCTION__, ##args)
#define BRICK_INF(fmt, args...) printk(BRICK_INFO  "%s(): " fmt, __FUNCTION__, ##args)
#ifdef BRICK_DEBUGGING
#define BRICK_DBG(fmt, args...) printk(BRICK_DEBUG "%s(): " fmt, __FUNCTION__, ##args)
#else
#define BRICK_DBG(args...) /**/
#endif

#define MAX_BRICK_TYPES 64

/////////////////////////////////////////////////////////////////////////

// definitions for generic objects with aspects

#define MAX_DEFAULT_ASPECTS 8

struct generic_aspect;

#define GENERIC_ASPECT_TYPE(BRICK)					\
	char *aspect_type_name;						\
	const struct generic_object_type *object_type;			\
	int aspect_size;						\
	int (*init_fn)(struct generic_aspect *ini, void *data);	\

struct generic_aspect_type {
	GENERIC_ASPECT_TYPE(generic);
};

#define GENERIC_ASPECT_LAYOUT(BRICK)					\
	const struct generic_aspect_type *aspect_type;			\
	void *init_data;						\
	int aspect_offset;						\

struct generic_aspect_layout {
	GENERIC_ASPECT_LAYOUT(generic);
};

#define GENERIC_OBJECT_TYPE(BRICK)					\
	char *object_type_name;						\
	int default_size;						\
	int brick_obj_nr;						\

struct generic_object_type {
	GENERIC_OBJECT_TYPE(generic);
};

#define GENERIC_OBJECT_LAYOUT(BRICK)					\
	struct generic_aspect_layout **aspect_layouts;			\
	const struct generic_object_type *object_type;			\
	int aspect_count;						\
	int aspect_max;							\
	int object_size;						\

struct generic_object_layout {
	GENERIC_OBJECT_LAYOUT(generic);
};

#define GENERIC_OBJECT(BRICK)						\
	struct BRICK##_object_layout *object_layout;			\
	int object_size;						\

struct generic_object {
	GENERIC_OBJECT(generic);
};

#define GENERIC_ASPECT(BRICK)						\
	struct BRICK##_object *object;					\

struct generic_aspect {
	GENERIC_ASPECT(generic);
};

/////////////////////////////////////////////////////////////////////////

// definitions for generic bricks

struct generic_input;
struct generic_output;
struct generic_brick_ops;
struct generic_output_ops;
struct generic_brick_type;

#define GENERIC_BRICK(BRICK)						\
	char *brick_name;						\
	const struct BRICK##_brick_type *type;				\
	struct BRICK##_brick_ops *ops;					\
	int nr_inputs;							\
	int nr_outputs;							\
	struct BRICK##_input **inputs;					\
	struct BRICK##_output **outputs;				\
	struct list_head tmp_head;					\

struct generic_brick {
	GENERIC_BRICK(generic);
};

#define GENERIC_INPUT(BRICK)						\
	char *input_name;						\
	struct BRICK##_brick *brick;					\
	const struct BRICK##_input_type *type;				\
	struct BRICK##_output *connect;				\
	struct list_head input_head;					\
	
struct generic_input {
	GENERIC_INPUT(generic);
};

#define GENERIC_OUTPUT(BRICK)						\
	char *output_name;						\
	struct BRICK##_brick *brick;					\
	const struct BRICK##_output_type *type;			\
	struct BRICK##_output_ops *ops;				\
	struct list_head output_head;					\
	int nr_connected;						\
	/* _must_ be the last member */					\
	struct generic_aspect_layout aspect_layouts[BRICK_OBJ_NR];	\
	
struct generic_output {
	GENERIC_OUTPUT(generic);
};

#define GENERIC_OUTPUT_CALL(OUTPUT,OP,ARGS...)				\
	(								\
		(void)LOCK_CHECK(OP),					\
		(OUTPUT) && (OUTPUT)->ops->OP ?				\
		(OUTPUT)->ops->OP(OUTPUT, ##ARGS) :			\
		-ENOSYS							\
	)
		
#define GENERIC_INPUT_CALL(INPUT,OP,ARGS...)				\
	(							        \
		(void)LOCK_CHECK(OP),					\
		(INPUT) && (INPUT)->connect ?				\
		GENERIC_OUTPUT_CALL((INPUT)->connect, OP, ##ARGS) :	\
		-ENOSYS							\
	)

#define GENERIC_BRICK_OPS(BRICK)					\
	/*int (*brick_start)(struct BRICK##_brick *brick);*/		\
	/*int (*brick_stop)(struct BRICK##_brick *brick);*/		\
	
struct generic_brick_ops {
	GENERIC_BRICK_OPS(generic);
};

#define GENERIC_OUTPUT_OPS(BRICK)					\
	/*int (*output_start)(struct BRICK##_output *output);*/	\
	/*int (*output_stop)(struct BRICK##_output *output);*/		\
	int (*make_object_layout)(struct BRICK##_output *output, struct generic_object_layout *object_layout); \
	
struct generic_output_ops {
	GENERIC_OUTPUT_OPS(generic)
};

// although possible, *_type should never be extended
#define GENERIC_BRICK_TYPE(BRICK)					\
	char *type_name;						\
	int brick_size;							\
	int max_inputs;							\
	int max_outputs;						\
	const struct BRICK##_input_type **default_input_types;		\
	char **default_input_names;					\
	const struct BRICK##_output_type **default_output_types;	\
	char **default_output_names;					\
	struct BRICK##_brick_ops *master_ops;				\
	const struct BRICK##input_types **default_type;		\
	int (*brick_construct)(struct BRICK##_brick *brick);		\
	int (*brick_destruct)(struct BRICK##_brick *brick);		\

struct generic_brick_type {
	GENERIC_BRICK_TYPE(generic);
};

#define GENERIC_INPUT_TYPE(BRICK)					\
	char *type_name;						\
	int input_size;							\
	int (*input_construct)(struct BRICK##_input *input);		\
	int (*input_destruct)(struct BRICK##_input *input);		\

struct generic_input_type {
	GENERIC_INPUT_TYPE(generic);
};

#define GENERIC_OUTPUT_TYPE(BRICK)					\
	char *type_name;						\
	int output_size;						\
	struct BRICK##_output_ops *master_ops;				\
	int (*output_construct)(struct BRICK##_output *output);	\
	int (*output_destruct)(struct BRICK##_output *output);		\
	const struct generic_aspect_type **aspect_types;		\
	const int layout_code[BRICK_OBJ_NR];				\

struct generic_output_type {
	GENERIC_OUTPUT_TYPE(generic);
};

#define LAYOUT_NONE         0
#define LAYOUT_ALL          -1
#define LAYOUT_1(X1)        ((X1) | 255 << 8)
#define LAYOUT_2(X1,X2)     ((X1) | (X2) << 8 | 255 << 16)
#define LAYOUT_3(X1,X2,X3)  ((X1) | (X2) << 8 | (X3) << 16 | 255 << 24)

int generic_register_brick_type(const struct generic_brick_type *new_type);
int generic_unregister_brick_type(const struct generic_brick_type *old_type);

#ifdef _STRATEGY // call this only in strategy bricks, never in ordinary bricks

// you need this only if you circumvent generic_brick_init_full()
extern inline int generic_brick_init(const struct generic_brick_type *type, struct generic_brick *brick, char *brick_name)
{
	brick->brick_name = brick_name;
	brick->type = type;
	brick->ops = type->master_ops;
	brick->nr_inputs = 0;
	brick->nr_outputs = 0;
	brick->tmp_head.next = brick->tmp_head.prev = &brick->tmp_head;
	return 0;
}

extern inline int generic_input_init(struct generic_brick *brick, int index, const struct generic_input_type *type, struct generic_input *input, char *input_name)
{
	if (index < 0 || index >= brick->type->max_inputs)
		return -ENOMEM;
	if (brick->inputs[index])
		return -EEXIST;
	input->input_name = input_name;
	input->brick = brick;
	input->type = type;
	input->connect = NULL;
	brick->inputs[index] = input;
	brick->nr_inputs++;
	return 0;
}

extern inline int generic_output_init(struct generic_brick *brick, int index, const struct generic_output_type *type, struct generic_output *output, char *output_name)
{
	if (index < 0 || index >= brick->type->max_outputs)
		return -ENOMEM;
	if (brick->outputs[index])
		return -EEXIST;
	output->output_name = output_name;
	output->brick = brick;
	output->type = type;
	output->ops = type->master_ops;
	output->nr_connected = 0;
	brick->outputs[index] = output;
	brick->nr_outputs++;
	return 0;
}

extern inline int generic_size(const struct generic_brick_type *brick_type)
{
	int size = brick_type->brick_size;
	int i;
	size += brick_type->max_inputs * sizeof(void*);
	for (i = 0; i < brick_type->max_inputs; i++) {
		size += brick_type->default_input_types[i]->input_size;
	}
	size += brick_type->max_outputs * sizeof(void*);
	for (i = 0; i < brick_type->max_outputs; i++) {
		size += brick_type->default_output_types[i]->output_size;
	}
	return size;
}

/* If possible, use this instead of generic_*_init().
 * input_types and output_types may be NULL => use default_*_types
 */
int generic_brick_init_full(
	void *data, 
	int size, 
	const struct generic_brick_type *brick_type,
	const struct generic_input_type **input_types,
	const struct generic_output_type **output_types,
	char **names);

int generic_brick_exit_full(
	struct generic_brick *brick);

extern inline int generic_connect(struct generic_input *input, struct generic_output *output)
{
	BRICK_DBG("generic_connect(input=%p, output=%p)\n", input, output);
	if (!input || !output)
		return -EINVAL;
	if (input->connect)
		return -EEXIST;
	input->connect = output;
	output->nr_connected++; //TODO: protect against races, e.g. atomic_t
	BRICK_DBG("now nr_connected=%d\n", output->nr_connected);
	return 0;
}

extern inline int generic_disconnect(struct generic_input *input)
{
	BRICK_DBG("generic_disconnect(input=%p)\n", input);
	if (!input)
		return -EINVAL;
	if (input->connect) {
		input->connect->nr_connected--; //TODO: protect against races, e.g. atomic_t
		BRICK_DBG("now nr_connected=%d\n", input->connect->nr_connected);
		input->connect = NULL;
	}
	return 0;
}

#endif // _STRATEGY

// simple wrappers for type safety
#define GENERIC_MAKE_FUNCTIONS(BRICK)					\
extern inline int BRICK##_register_brick_type(void)		        \
{									\
	extern const struct BRICK##_brick_type BRICK##_brick_type;	\
	extern int BRICK##_brick_nr;					\
	if (BRICK##_brick_nr >= 0) {					\
		BRICK_ERR("brick type " #BRICK " is already registered.\n"); \
		return -EEXIST;						\
	}								\
	BRICK##_brick_nr = generic_register_brick_type((const struct generic_brick_type*)&BRICK##_brick_type); \
	return BRICK##_brick_nr < 0 ? BRICK##_brick_nr : 0;		\
}									\
									\
extern inline int BRICK##_unregister_brick_type(void)		        \
{									\
	extern const struct BRICK##_brick_type BRICK##_brick_type;	\
	return generic_unregister_brick_type((const struct generic_brick_type*)&BRICK##_brick_type); \
}									\
									\
extern int BRICK##_make_object_layout(struct BRICK##_output *output, struct generic_object_layout *object_layout) \
{									\
	return default_make_object_layout((struct generic_output*)output, object_layout); \
}									\
									\
_STRATEGY_CODE(							        \
extern const struct BRICK##_brick_type BRICK##_brick_type;	        \
extern const struct BRICK##_input_type BRICK##_input_type;		\
extern const struct BRICK##_output_type BRICK##_output_type;	        \
									\
static inline int BRICK##_brick_init(struct BRICK##_brick *brick, char *brick_name) \
{									\
	return generic_brick_init((const struct generic_brick_type*)&BRICK##_brick_type, (struct generic_brick*)brick, brick_name); \
}									\
									\
static inline int BRICK##_input_init(struct BRICK##_brick *brick, int index, struct BRICK##_input *input, char *input_name) \
{									\
	return generic_input_init(					\
		(struct generic_brick*)brick,				\
		index,							\
		(struct generic_input_type*)&BRICK##_input_type,	\
		(struct generic_input*)input,				\
		input_name);						\
}									\
									\
static inline int BRICK##_output_init(struct BRICK##_brick *brick, int index, struct BRICK##_input *output, char *output_name) \
{									\
	return generic_output_init(					\
		(struct generic_brick*)brick,				\
		index,							\
		(const struct generic_output_type*)&BRICK##_output_type, \
		(struct generic_output*)output,				\
		output_name);						\
}									\
									\
extern inline int BRICK##_size(const struct BRICK##_brick_type *brick_type) \
{									\
	return generic_size((const struct generic_brick_type*)brick_type); \
}									\
									\
extern inline int BRICK##_brick_init_full(			        \
	void *data,							\
	int size,							\
	const struct BRICK##_brick_type *brick_type,			\
	const struct BRICK##_input_type **input_types,			\
	const struct BRICK##_output_type **output_types,		\
	char **names)							\
{									\
	return generic_brick_init_full(					\
		data,							\
		size,							\
		(const struct generic_brick_type*)brick_type,		\
		(const struct generic_input_type**)input_types,		\
		(const struct generic_output_type**)output_types,	\
		(char**)names);						\
}									\
									\
extern inline int BRICK##_brick_exit_full(			        \
	struct BRICK##_brick *brick)					\
{									\
	return generic_brick_exit_full(					\
		(struct generic_brick*)brick);				\
}									\
)

/* Define a pair of connectable subtypes.
 * For type safety, use this for all possible combinations.
 * Yes, this may become quadratic in large type systems, but
 * (a) thou shalt not define many types,
 * (b) these macros generate only definitions, but no additional 
 * code at runtime.
 */
#define GENERIC_MAKE_CONNECT(INPUT_BRICK,OUTPUT_BRICK)		\
									\
_STRATEGY_CODE(							        \
									\
extern inline int INPUT_BRICK##_##OUTPUT_BRICK##_connect(	        \
	struct INPUT_BRICK##_input *input,				\
	struct OUTPUT_BRICK##_output *output)				\
{									\
	return generic_connect((struct generic_input*)input, (struct generic_output*)output); \
}									\
									\
extern inline int INPUT_BRICK##_##OUTPUT_BRICK####_disconnect(        \
	struct INPUT_BRICK##_input *input)				\
{									\
	return generic_disconnect((struct generic_input*)input);	\
}									\
)

///////////////////////////////////////////////////////////////////////

// default operations on objects / aspects

extern int default_make_object_layout(struct generic_output *output, struct generic_object_layout *object_layout);

extern inline int generic_add_aspect(struct generic_output *output, struct generic_object_layout *object_layout, const struct generic_aspect_type *aspect_type)
{
	struct generic_aspect_layout *aspect_layout;
	int nr;

	if (unlikely(!object_layout->object_type)) {
		return -EINVAL;
	}
	nr = object_layout->object_type->brick_obj_nr;
	aspect_layout = (void*)&output->aspect_layouts[nr];
	if (aspect_layout->aspect_type) {
		/* aspect_layout is already initialized.
		 * this is a kind of "dynamic programming".
		 * ensure consistency to last call.
		 */
		int min_offset;
		if (aspect_layout->aspect_type != aspect_type) {
			BRICK_ERR("inconsistent use of aspect_type %s != %s\n", aspect_type->aspect_type_name, aspect_layout->aspect_type->aspect_type_name);
			return -EBADF;
		}
		if (aspect_layout->init_data != output) {
			BRICK_ERR("inconsistent output assigment (aspect_type=%s)\n", aspect_type->aspect_type_name);
			return -EBADF;
		}
		min_offset = aspect_layout->aspect_offset + aspect_type->aspect_size;
		if (object_layout->object_size > min_offset) {
			BRICK_ERR("overlapping aspects %d > %d (aspect_type=%s)\n", object_layout->object_size, min_offset, aspect_type->aspect_type_name);
			return -ENOMEM;
		}
		BRICK_DBG("adjusting object_size %d to %d (aspect_type=%s)\n", object_layout->object_size, min_offset, aspect_type->aspect_type_name);
		object_layout->object_size = min_offset;
	} else {
		/* first call: initialize aspect_layout. */
		aspect_layout->aspect_type = aspect_type;
		aspect_layout->init_data = output;
		aspect_layout->aspect_offset = object_layout->object_size;
		object_layout->object_size += aspect_type->aspect_size;
	}
	nr = object_layout->aspect_count++;
	object_layout->aspect_layouts[nr] = aspect_layout;
	return 0;
}

extern int default_init_object_layout(struct generic_output *output, struct generic_object_layout *object_layout, int aspect_max, const struct generic_object_type *object_type);

extern struct generic_object *alloc_generic(struct generic_output *output, struct generic_object_layout *object_layout);

extern void free_generic(struct generic_object *object);

#define GENERIC_OBJECT_LAYOUT_FUNCTIONS(BRICK)				\
									\
extern inline int BRICK##_init_object_layout(struct BRICK##_output *output, struct generic_object_layout *object_layout, int aspect_max, const struct generic_object_type *object_type) \
{									\
	if (likely(object_layout->object_type))				\
		return 0;						\
	return default_init_object_layout((struct generic_output*)output, object_layout, aspect_max, object_type); \
}									\

#define GENERIC_ASPECT_LAYOUT_FUNCTIONS(BRICK,PREFIX)			\
									\
extern inline int BRICK##_##PREFIX##_add_aspect(struct BRICK##_output *output, struct PREFIX##_object_layout *object_layout, const struct generic_aspect_type *aspect_type) \
{									\
	int res = generic_add_aspect((struct generic_output*)output, (struct generic_object_layout *)object_layout, aspect_type); \
	BRICK_DBG(#BRICK " " #PREFIX "added aspect_type %p (%s) to object_layout %p (type %s) on output %p (type %s), status=%d\n", aspect_type, aspect_type->aspect_type_name, object_layout, object_layout->object_type->object_type_name, output, output->type->type_name, res); \
	return res;							\
}									\

#define GENERIC_OBJECT_FUNCTIONS(BRICK)				\
									\
extern inline struct BRICK##_object *BRICK##_construct(void *data, struct BRICK##_object_layout *object_layout) \
{									\
	struct BRICK##_object *obj = data;				\
	int i;								\
									\
	obj->object_layout = object_layout;				\
	for (i = 0; i < object_layout->aspect_count; i++) {		\
		struct generic_aspect_layout *aspect_layout;		\
		struct generic_aspect *aspect;				\
		aspect_layout = object_layout->aspect_layouts[i];	\
		if (!aspect_layout->aspect_type)				\
			continue;					\
		aspect = data + aspect_layout->aspect_offset;		\
		aspect->object = (void*)obj;				\
		if (aspect_layout->aspect_type->init_fn) {		\
			int status = aspect_layout->aspect_type->init_fn((void*)aspect, aspect_layout->init_data); \
			if (status) {					\
				return NULL;				\
			}						\
		}							\
	}								\
	return obj;							\
}									\

#define GENERIC_ASPECT_FUNCTIONS(BRICK,PREFIX)				\
									\
extern inline struct BRICK##_##PREFIX##_aspect *BRICK##_##PREFIX##_get_aspect(struct BRICK##_output *output, struct PREFIX##_object *obj) \
{									\
	struct PREFIX##_object_layout *object_layout;			\
	struct generic_aspect_layout *aspect_layout;			\
	int nr;								\
									\
	object_layout = obj->object_layout;				\
	nr = object_layout->object_type->brick_obj_nr;			\
	aspect_layout = &output->aspect_layouts[nr];			\
	if (unlikely(!aspect_layout->aspect_type)) {			\
		BRICK_ERR("brick "#BRICK": bad aspect slot on " #PREFIX " pointer %p\n", obj); \
		return NULL;						\
	}								\
	return (void*)obj + aspect_layout->aspect_offset;		\
}									\
									\
extern inline int BRICK##_##PREFIX##_init_object_layout(struct BRICK##_output *output, struct generic_object_layout *object_layout) \
{									\
	return BRICK##_init_object_layout(output, object_layout, 16, &PREFIX##_type); \
}									\
									\
extern inline struct PREFIX##_object *BRICK##_alloc_##PREFIX(struct BRICK##_output *output, struct generic_object_layout *object_layout) \
{									\
	int status = BRICK##_##PREFIX##_init_object_layout(output, object_layout);	\
	if (status < 0)							\
		return NULL;						\
	return (struct PREFIX##_object*)alloc_generic((struct generic_output*)output, object_layout); \
}									\
									\
extern inline void BRICK##_free_##PREFIX(struct PREFIX##_object *object)     \
{									\
	free_generic((struct generic_object*)object);			\
}									\


GENERIC_OBJECT_LAYOUT_FUNCTIONS(generic);
GENERIC_OBJECT_FUNCTIONS(generic);

///////////////////////////////////////////////////////////////////////

// some helpers

#ifdef CONFIG_DEBUG_SPINLOCK

# define LOCK_CHECK(OP)					\
	({						\
		if (atomic_read(&current->lock_count)) {	\
			BRICK_ERR("never call " #OP "() with a spinlock held.\n"); \
		}					\
	})

# define traced_lock(spinlock,flags)			\
	do {						\
		if (atomic_read(&current->lock_count)) {	\
			BRICK_ERR("please do not nest spinlocks at line %d, reorganize your code.\n", __LINE__); \
		}					\
		atomic_inc(&current->lock_count);	\
		/*spin_lock_irqsave(spinlock,flags);*/	\
		(void)flags;				\
		spin_lock(spinlock);			\
	} while (0)

# define traced_unlock(spinlock,flags)				\
	do {							\
		/*spin_unlock_irqrestore(spinlock,flags);*/	\
		spin_unlock(spinlock);				\
		atomic_dec(&current->lock_count);		\
	} while (0)

#else

# define LOCK_CHECK(OP) 0
# define traced_lock(spinlock,flags)   spin_lock_irqsave(spinlock,flags)
# define traced_unlock(spinlock,flags) spin_unlock_irqrestore(spinlock,flags)

#endif
#endif