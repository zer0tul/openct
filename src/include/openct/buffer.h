/*
 * Buffer handling functions of the IFD handler library
 *
 * Copyright (C) 2003, Olaf Kirch <okir@suse.de>
 */

#ifndef IFD_BUFFER_H
#define IFD_BUFFER_H

#include <sys/types.h>

typedef struct ct_buf {
	unsigned char *		base;
	unsigned int		head, tail, size;
} ct_buf_t;

extern void		ct_buf_init(ct_buf_t *, void *, size_t);
extern void		ct_buf_set(ct_buf_t *, void *, size_t);
extern void		ct_buf_clear(ct_buf_t *);
extern int		ct_buf_get(ct_buf_t *, void *, size_t);
extern int		ct_buf_put(ct_buf_t *, const void *, size_t);
extern int		ct_buf_putc(ct_buf_t *, int);
extern unsigned int	ct_buf_avail(ct_buf_t *);
extern unsigned int	ct_buf_tailroom(ct_buf_t *);
extern unsigned int	ct_buf_size(ct_buf_t *);
extern void *		ct_buf_head(ct_buf_t *);
extern void *		ct_buf_tail(ct_buf_t *);
extern int		ct_buf_read(ct_buf_t *, int);
extern void		ct_buf_compact(ct_buf_t *);


#endif /* IFD_BUFFER_H */