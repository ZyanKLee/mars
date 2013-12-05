// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_PROC_H
#define MARS_PROC_H

typedef char * (*xio_info_fn)(void);

extern xio_info_fn xio_info;

/////////////////////////////////////////////////////////////////////////

// init

extern int init_xio_proc(void);
extern void exit_xio_proc(void);

#endif
