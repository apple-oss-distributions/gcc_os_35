/* Threads compatibility routines for libgcc2.  */
/* Compile this one with gcc.  */
/* Copyright (C) 2003 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.
GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* As a special exception, if you link this library with other files,
   some of which are compiled with GCC, to produce an executable,
   this library does not by itself cause the resulting executable
   to be covered by the GNU General Public License.
   This exception does not however invalidate any other reasons why
   the executable file might be covered by the GNU General Public License.  */

#ifndef GCC_GTHR_GNAT_H
#define GCC_GTHR_GNAT_H

#pragma GCC visibility push(default)

/* Just provide compatibility for mutex handling.  */

typedef int __gthread_mutex_t;

#define __GTHREAD_MUTEX_INIT 0

extern void __gnat_install_locks (void (*lock) (void), void (*unlock) (void));
extern int __gthread_active_p (void);
extern int __gthread_mutex_lock (__gthread_mutex_t *);
extern int __gthread_mutex_unlock (__gthread_mutex_t *);

#pragma GCC visibility pop

#endif /* ! GCC_GTHR_GNAT_H */

