/* This file exists soley to keep Metrowerks' compilers happy.  The version
   used by GCC 3.4 and later can be found in /usr/lib/gcc, although it's
   not very informative.  */

#ifndef _FLOAT_H_
#define _FLOAT_H_

#ifndef __MWERKS__
#error This file is only for Metrowerks compatibilty.
#endif

/* Define various characteristics of floating-point types, if needed.  */
#ifndef __FLT_RADIX__   
#define __FLT_RADIX__ 2
#endif
#ifndef __FLT_MANT_DIG__
#define __FLT_MANT_DIG__ 24
#endif
#ifndef __FLT_DIG__
#define __FLT_DIG__ 6
#endif
#ifndef __FLT_EPSILON__
#define __FLT_EPSILON__ 1.19209290e-07F
#endif
#ifndef __FLT_MIN__
#define __FLT_MIN__ 1.17549435e-38F
#endif
#ifndef __FLT_MAX__
#define __FLT_MAX__ 3.40282347e+38F
#endif
#ifndef __FLT_MIN_EXP__
#define __FLT_MIN_EXP__ (-125)
#endif
#ifndef __FLT_MIN_10_EXP__
#define __FLT_MIN_10_EXP__ (-37)
#endif
#ifndef __FLT_MAX_EXP__
#define __FLT_MAX_EXP__ 128
#endif
#ifndef __FLT_MAX_10_EXP__
#define __FLT_MAX_10_EXP__ 38
#endif
#ifndef __DBL_MANT_DIG__
#define __DBL_MANT_DIG__ 53
#endif
#ifndef __DBL_DIG__
#define __DBL_DIG__ 15
#endif
#ifndef __DBL_EPSILON__
#define __DBL_EPSILON__ 2.2204460492503131e-16
#endif
#ifndef __DBL_MIN__
#define __DBL_MIN__ 2.2250738585072014e-308
#endif
#ifndef __DBL_MAX__
#define __DBL_MAX__ 1.7976931348623157e+308
#endif
#ifndef __DBL_MIN_EXP__
#define __DBL_MIN_EXP__ (-1021)
#endif
#ifndef __DBL_MIN_10_EXP__
#define __DBL_MIN_10_EXP__ (-307)
#endif
#ifndef __DBL_MAX_EXP__
#define __DBL_MAX_EXP__ 1024
#endif
#ifndef __DBL_MAX_10_EXP__
#define __DBL_MAX_10_EXP__ 308
#endif
#ifndef __LDBL_MANT_DIG__
#define __LDBL_MANT_DIG__ 53
#endif
#ifndef __LDBL_DIG__  
#define __LDBL_DIG__ 15 
#endif
#ifndef __LDBL_EPSILON__
#define __LDBL_EPSILON__ 2.2204460492503131e-16
#endif
#ifndef __LDBL_MIN__
#define __LDBL_MIN__ 2.2250738585072014e-308
#endif
#ifndef __LDBL_MAX__
#define __LDBL_MAX__ 1.7976931348623157e+308
#endif
#ifndef __LDBL_MIN_EXP__
#define __LDBL_MIN_EXP__ (-1021)
#endif
#ifndef __LDBL_MIN_10_EXP__
#define __LDBL_MIN_10_EXP__ (-307)
#endif
#ifndef __LDBL_MAX_EXP__
#define __LDBL_MAX_EXP__ 1024
#endif
#ifndef __LDBL_MAX_10_EXP__
#define __LDBL_MAX_10_EXP__ 308
#endif

#endif
