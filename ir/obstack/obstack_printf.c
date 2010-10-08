#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "obstack.h"

#ifdef _WIN32
/* win32/C89 has no va_copy function... so we have to use the stupid fixed-length version */
int obstack_vprintf(struct obstack *obst, const char *fmt, va_list ap)
{
	char buf[16384];
	int len = _vsnprintf(buf, sizeof(buf), fmt, ap);
	obstack_grow(obst, buf, len);
	return len;
}
#else
int obstack_vprintf(struct obstack *obst, const char *fmt, va_list ap)
{
	char    buf[128];
	char   *buffer = buf;
	size_t  size   = sizeof(buf);
	int     len;

	for (;;) {
		va_list tap;
		va_copy(tap, ap);
		len = vsnprintf(buffer, size, fmt, tap);

		/* snprintf should return -1 only in the error case, but older glibcs
		 * and probably other systems are buggy in this respect and return -1 if
		 * the buffer was too small. We only abort for LARGE unrealistic buffer
		 * sizes here */
		if (len < 0) {
			if (buffer != buf)
				free(buffer);
			if (size > 65536)
				return -1;
			size *= 2;
		} else if ((size_t)len >= size) {
			/* If we come here more than once, vsnprintf() returned garbage */
			assert(buffer == buf);
			size = (size_t)len + 1;
		} else {
			break;
		}
		buffer = malloc(size);
	}

	obstack_grow(obst, buffer, len);
	if (buffer != buf)
		free(buffer);

	return len;
}
#endif

int obstack_printf(struct obstack *obst, const char *fmt, ...)
{
	va_list ap;
	int     res;

	va_start(ap, fmt);
	res = obstack_vprintf(obst, fmt, ap);
	va_end(ap);

	return res;
}
