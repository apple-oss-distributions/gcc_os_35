/* This is the `system' limits.h, independent of any particular
   compiler.  GCC provides its own limits.h which can be found in
   /usr/lib/gcc, although it is not very informative.  
   This file is public domain.  */
#if defined (__ppc__)
#include <ppc/limits.h>
#elif defined (__i386__)
#include <i386/limits.h>
#else
#error architecture not supported
#endif
