// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef XIO_USEBUF_H
#define XIO_USEBUF_H

struct usebuf_aio_aspect {
	GENERIC_ASPECT(aio);
	struct usebuf_aio_aspect *sub_aio_a;
	struct usebuf_input *input;
#if 1
	int yyy;
#endif
};

struct usebuf_brick {
	XIO_BRICK(usebuf);
};

struct usebuf_input {
	XIO_INPUT(usebuf);
};

struct usebuf_output {
	XIO_OUTPUT(usebuf);
};

XIO_TYPES(usebuf);

#endif
