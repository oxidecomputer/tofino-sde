void *stdout;

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>


void
__assert_fail(const char * assertion, const char * file, unsigned int line, const char * function)
{
	__assert_c99(assertion, file, line, function);
}

int
__isoc99_sscanf(const char *s, const char *fmt, ...)
{
	int ret;
	va_list ap;
   
	va_start(ap, fmt);
	ret = vscanf(fmt, ap);
	va_end(ap);

	return (ret);
}

// This seems to be a glibc-specific function and is only called from the ucli
// code to support Tofino 1.
unsigned short int** __ctype_b_loc (void)
{
	fprintf(stderr,
	    "__ctype_b_loc not implemented in libport_mgr_hw.a shim\n");
	abort();
}
