// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_DUMMY_H
#define MARS_DUMMY_H

struct dummy_aio_aspect {
	GENERIC_ASPECT(aio);
	int my_own;
};

struct dummy_brick {
	MARS_BRICK(dummy);
	int my_own;
};

struct dummy_input {
	MARS_INPUT(dummy);
};

struct dummy_output {
	MARS_OUTPUT(dummy);
	int my_own;
};

MARS_TYPES(dummy);

#endif
