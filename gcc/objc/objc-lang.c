/* Language-dependent hooks for Objective-C.
   Copyright 2001, 2002, 2003, 2004 Free Software Foundation, Inc.
   Contributed by Ziemowit Laski  <zlaski@apple.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "c-tree.h"
#include "c-common.h"
#include "ggc.h"
#include "objc-act.h"
#include "langhooks.h"
#include "langhooks-def.h"
#include "diagnostic.h"
#include "c-pretty-print.h"
#include "c-objc-common.h"

enum c_language_kind c_language = clk_objc;

/* Lang hooks common to C and ObjC are declared in c-objc-common.h;
   consequently, there should be very few hooks below.  */

#undef LANG_HOOKS_NAME
#define LANG_HOOKS_NAME "GNU Objective-C"
#undef LANG_HOOKS_INIT
#define LANG_HOOKS_INIT objc_init
#undef LANG_HOOKS_DECL_PRINTABLE_NAME
#define LANG_HOOKS_DECL_PRINTABLE_NAME objc_printable_name

/* Each front end provides its own lang hook initializer.  */
const struct lang_hooks lang_hooks = LANG_HOOKS_INITIALIZER;

/* Table indexed by tree code giving a string containing a character
   classifying the tree code.  */

#define DEFTREECODE(SYM, NAME, TYPE, LENGTH) TYPE,

const enum tree_code_class tree_code_type[] = {
#include "tree.def"
  tcc_exceptional,
#include "c-common.def"
  tcc_exceptional,
#include "objc-tree.def"
};
#undef DEFTREECODE

/* Table indexed by tree code giving number of expression
   operands beyond the fixed part of the node structure.
   Not used for types or decls.  */

#define DEFTREECODE(SYM, NAME, TYPE, LENGTH) LENGTH,

const unsigned char tree_code_length[] = {
#include "tree.def"
  0,
#include "c-common.def"
  0,
#include "objc-tree.def"
};
#undef DEFTREECODE

/* Names of tree components.
   Used for printing out the tree and error messages.  */
#define DEFTREECODE(SYM, NAME, TYPE, LEN) NAME,

const char * const tree_code_name[] = {
#include "tree.def"
  "@@dummy",
#include "c-common.def"
  "@@dummy",
#include "objc-tree.def"
};
#undef DEFTREECODE

/* Lang hook routines common to C and ObjC appear in c-objc-common.c;
   there should be very few (if any) routines below.  */

void
finish_file (void)
{
  objc_finish_file ();
}

#include "gtype-objc.h"
