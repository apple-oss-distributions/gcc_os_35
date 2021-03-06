/* General-purpose hooks.
   Copyright (C) 2002, 2003, 2004 Free Software Foundation, Inc.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

 In other words, you are welcome to use, share and improve this program.
 You are forbidden to forbid anyone else to use, share and improve
 what you give them.   Help stamp out software-hoarding!  */

#ifndef GCC_HOOKS_H
#define GCC_HOOKS_H

#include "machmode.h"

extern bool hook_bool_void_false (void);
extern bool hook_bool_void_true (void);
extern bool hook_bool_bool_false (bool);
extern bool hook_bool_mode_false (enum machine_mode);
extern bool hook_bool_tree_false (tree);
extern bool hook_bool_tree_true (tree);
extern bool hook_bool_tree_hwi_hwi_tree_false (tree, HOST_WIDE_INT, HOST_WIDE_INT,
					tree);
extern bool hook_bool_tree_hwi_hwi_tree_true (tree, HOST_WIDE_INT, HOST_WIDE_INT,
				       tree);
extern bool hook_bool_rtx_false (rtx);
extern bool hook_bool_uintp_uintp_false (unsigned int *, unsigned int *);
extern bool hook_bool_rtx_int_int_intp_false (rtx, int, int, int *);
extern bool hook_bool_constcharptr_size_t_false (const char *, size_t);

extern void hook_void_void (void);
extern void hook_void_int (int);
extern void hook_void_constcharptr (const char *);
extern void hook_void_FILEptr_constcharptr (FILE *, const char *);
extern void hook_void_tree (tree);
extern void hook_void_tree_treeptr (tree, tree *);

extern int hook_int_tree_0 (tree);
extern int hook_int_tree_tree_1 (tree, tree);
extern int hook_int_rtx_0 (rtx);
extern int hook_int_size_t_constcharptr_int_0 (size_t, const char *, int);
extern int hook_int_void_no_regs (void);
extern int hook_int_void_1 (void);

extern unsigned hook_uint_uint_constcharptrptr_0 (unsigned, const char **);

extern bool default_can_output_mi_thunk_no_vcall (tree, HOST_WIDE_INT,
					   HOST_WIDE_INT, tree);

extern bool hook_bool_tree_tree_false (tree, tree);

extern rtx hook_rtx_rtx_identity (rtx);
extern rtx hook_rtx_rtx_null (rtx);
extern rtx hook_rtx_tree_int_null (tree, int);
extern tree hook_tree_tree_identity (tree a);
extern const char *hook_constcharptr_tree_null (tree);
/* APPLE LOCAL begin constant cfstrings */
extern tree hook_tree_tree_null (tree);
extern tree hook_tree_tree_tree_tree_null (tree, tree, tree);
/* APPLE LOCAL end constant cfstrings */
extern tree hook_tree_tree_bool_null (tree, bool);
#endif
