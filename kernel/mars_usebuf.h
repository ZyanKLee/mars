// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_USEBUF_H
#define MARS_USEBUF_H

struct usebuf_aio_aspect {
	GENERIC_ASPECT(aio);
	struct usebuf_aio_aspect *sub_aio_a;
	struct usebuf_input *input;
#if 1
	int yyy;
#endif
};

struct usebuf_brick {
	MARS_BRICK(usebuf);
};

struct usebuf_input {
	MARS_INPUT(usebuf);
};

struct usebuf_output {
	MARS_OUTPUT(usebuf);
};

MARS_TYPES(usebuf);

#endif
