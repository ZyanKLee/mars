// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

#ifndef LIB_QUEUE_H
#define LIB_QUEUE_H

#define QUEUE_ANCHOR(PREFIX,KEYTYPE,HEAPTYPE)				\
	/* parameters */						\
	/* readonly from outside */					\
	atomic_t q_queued;						\
	atomic_t q_flying;						\
	atomic_t q_total;						\
	/* tunables */							\
	int q_batchlen; 						\
	int q_io_prio;							\
	bool q_ordering;						\
	/* private */							\
	wait_queue_head_t *q_event;					\
	spinlock_t q_lock;						\
	struct list_head q_anchor;					\
	struct pairing_heap_##HEAPTYPE *heap_high;			\
	struct pairing_heap_##HEAPTYPE *heap_low;			\
	long long q_last_insert; /* jiffies */				\
	KEYTYPE heap_margin;						\
	KEYTYPE last_pos;						\

#define QUEUE_FUNCTIONS(PREFIX,ELEM_TYPE,HEAD,KEYFN,KEYCMP,HEAPTYPE)	\
									\
static inline								\
void q_##PREFIX##_trigger(struct PREFIX##_queue *q)			\
{									\
	if (q->q_event) {						\
		wake_up_interruptible(q->q_event);			\
	}								\
}									\
									\
static inline								\
void q_##PREFIX##_init(struct PREFIX##_queue *q)			\
{									\
	INIT_LIST_HEAD(&q->q_anchor);					\
	q->heap_low = NULL;						\
	q->heap_high = NULL;						\
	spin_lock_init(&q->q_lock);					\
	atomic_set(&q->q_queued, 0);					\
	atomic_set(&q->q_flying, 0);					\
}									\
									\
static inline								\
void q_##PREFIX##_insert(struct PREFIX##_queue *q, ELEM_TYPE *elem)	\
{									\
	spin_lock(&q->q_lock);						\
									\
	if (q->q_ordering) {						\
		struct pairing_heap_##HEAPTYPE **use = &q->heap_high;	\
		if (KEYCMP(KEYFN(elem), &q->heap_margin) <= 0) {	\
			use = &q->heap_low;				\
		}							\
		ph_insert_##HEAPTYPE(use, &elem->ph);			\
	} else {							\
		list_add_tail(&elem->HEAD, &q->q_anchor);		\
	}								\
	atomic_inc(&q->q_queued);					\
	atomic_inc(&q->q_total);					\
	q->q_last_insert = jiffies;					\
									\
	spin_unlock(&q->q_lock);					\
									\
	q_##PREFIX##_trigger(q);					\
}									\
									\
static inline								\
void q_##PREFIX##_pushback(struct PREFIX##_queue *q, ELEM_TYPE *elem)	\
{									\
	if (q->q_ordering) {						\
		atomic_dec(&q->q_total);				\
		q_##PREFIX##_insert(q, elem);				\
		return; 						\
	}								\
									\
	spin_lock(&q->q_lock);						\
									\
	list_add(&elem->HEAD, &q->q_anchor);				\
	atomic_inc(&q->q_queued);					\
									\
	spin_unlock(&q->q_lock);					\
}									\
									\
static inline								\
ELEM_TYPE *q_##PREFIX##_fetch(struct PREFIX##_queue *q) 		\
{									\
	ELEM_TYPE *elem = NULL; 					\
									\
	spin_lock(&q->q_lock);						\
									\
	if (q->q_ordering) {						\
		if (!q->heap_high) {					\
			q->heap_high = q->heap_low;			\
			q->heap_low = NULL;				\
			q->heap_margin = 0;				\
			q->last_pos = 0;				\
		}							\
		if (q->heap_high) {					\
			elem = container_of(q->heap_high, ELEM_TYPE, ph);\
									\
			if (unlikely(KEYCMP(KEYFN(elem), &q->last_pos) < 0)) {\
				XIO_ERR("backskip pos %lld -> %lld\n", (long long)q->last_pos, (long long)KEYFN(elem));\
			}						\
			memcpy(&q->last_pos, KEYFN(elem), sizeof(q->last_pos));\
									\
			if (KEYCMP(KEYFN(elem), &q->heap_margin) > 0) { \
				memcpy(&q->heap_margin, KEYFN(elem), sizeof(q->heap_margin));\
			}						\
			ph_delete_min_##HEAPTYPE(&q->heap_high);	\
			atomic_dec(&q->q_queued);			\
		}							\
	} else if (!list_empty(&q->q_anchor)) { 			\
		struct list_head *next = q->q_anchor.next;		\
		list_del_init(next);					\
		atomic_dec(&q->q_queued);				\
		elem = container_of(next, ELEM_TYPE, HEAD);		\
	}								\
									\
	spin_unlock(&q->q_lock);					\
									\
	q_##PREFIX##_trigger(q);					\
									\
	return elem;							\
}									\
									\
static inline								\
void q_##PREFIX##_inc_flying(struct PREFIX##_queue *q)			\
{									\
	atomic_inc(&q->q_flying);					\
	q_##PREFIX##_trigger(q);					\
}									\
									\
static inline								\
void q_##PREFIX##_dec_flying(struct PREFIX##_queue *q)			\
{									\
	atomic_dec(&q->q_flying);					\
	q_##PREFIX##_trigger(q);					\
}									\
									\


#endif
