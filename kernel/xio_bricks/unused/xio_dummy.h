// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef XIO_DUMMY_H
#define XIO_DUMMY_H

struct dummy_aio_aspect {
	GENERIC_ASPECT(aio);
	int my_own;
};

struct dummy_brick {
	XIO_BRICK(dummy);
	int my_own;
};

struct dummy_input {
	XIO_INPUT(dummy);
};

struct dummy_output {
	XIO_OUTPUT(dummy);
	int my_own;
};

XIO_TYPES(dummy);

#endif
