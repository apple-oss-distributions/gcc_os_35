/* Mudflap: narrow-pointer bounds-checking by tree rewriting.
   Copyright (C) 2002, 2003, 2004 Free Software Foundation, Inc.
   Contributed by Frank Ch. Eigler <fche@redhat.com>
   and Graydon Hoare <graydon@redhat.com>

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


#include "config.h"
#include "errors.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "hard-reg-set.h"
#include "rtl.h"
#include "tree.h"
#include "tm_p.h"
#include "basic-block.h"
#include "flags.h"
#include "function.h"
#include "tree-inline.h"
#include "tree-gimple.h"
#include "tree-flow.h"
#include "tree-mudflap.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "hashtab.h"
#include "diagnostic.h"
#include <demangle.h>
#include "langhooks.h"
#include "ggc.h"
#include "cgraph.h"

/* Internal function decls */

/* Helpers.  */
static tree mf_build_string (const char *string);
static tree mf_varname_tree (tree);
static tree mf_file_function_line_tree (location_t);

/* Indirection-related instrumentation.  */
static void mf_decl_cache_locals (void);
static void mf_decl_clear_locals (void);
static void mf_xform_derefs (void);
static void execute_mudflap_function_ops (void);

/* Addressable variables instrumentation.  */
static void mf_xform_decls (tree, tree);
static tree mx_xfn_xform_decls (tree *, int *, void *);
static void mx_register_decls (tree, tree *);
static void execute_mudflap_function_decls (void);


/* ------------------------------------------------------------------------ */
/* Some generally helpful functions for mudflap instrumentation.  */

/* Build a reference to a literal string.  */
static tree
mf_build_string (const char *string)
{
  size_t len = strlen (string);
  tree result = mf_mark (build_string (len + 1, string));

  TREE_TYPE (result) = build_array_type
    (char_type_node, build_index_type (build_int_cst (NULL_TREE, len)));
  TREE_CONSTANT (result) = 1;
  TREE_INVARIANT (result) = 1;
  TREE_READONLY (result) = 1;
  TREE_STATIC (result) = 1;

  result = build1 (ADDR_EXPR, build_pointer_type (char_type_node), result);

  return mf_mark (result);
}

/* Create a properly typed STRING_CST node that describes the given
   declaration.  It will be used as an argument for __mf_register().
   Try to construct a helpful string, including file/function/variable
   name.  */

static tree
mf_varname_tree (tree decl)
{
  static pretty_printer buf_rec;
  static int initialized = 0;
  pretty_printer *buf = & buf_rec;
  const char *buf_contents;
  tree result;

  gcc_assert (decl);

  if (!initialized)
    {
      pp_construct (buf, /* prefix */ NULL, /* line-width */ 0);
      initialized = 1;
    }
  pp_clear_output_area (buf);

  /* Add FILENAME[:LINENUMBER[:COLUMNNUMBER]].  */
  {
    expanded_location xloc = expand_location (DECL_SOURCE_LOCATION (decl));
    const char *sourcefile;
    unsigned sourceline = xloc.line;
    unsigned sourcecolumn = 0;
#ifdef USE_MAPPED_LOCATION
    sourcecolumn = xloc.column;
#endif
    sourcefile = xloc.file;
    if (sourcefile == NULL && current_function_decl != NULL_TREE)
      sourcefile = DECL_SOURCE_FILE (current_function_decl);
    if (sourcefile == NULL)
      sourcefile = "<unknown file>";

    pp_string (buf, sourcefile);

    if (sourceline != 0)
      {
        pp_string (buf, ":");
        pp_decimal_int (buf, sourceline);

        if (sourcecolumn != 0)
          {
            pp_string (buf, ":");
            pp_decimal_int (buf, sourcecolumn);
          }
      }
  }

  if (current_function_decl != NULL_TREE)
    {
      /* Add (FUNCTION) */
      pp_string (buf, " (");
      {
        const char *funcname = NULL;
        if (DECL_NAME (current_function_decl))
          funcname = lang_hooks.decl_printable_name (current_function_decl, 1);
        if (funcname == NULL)
          funcname = "anonymous fn";

        pp_string (buf, funcname);
      }
      pp_string (buf, ") ");
    }
  else
    pp_string (buf, " ");

  /* Add <variable-declaration>, possibly demangled.  */
  {
    const char *declname = NULL;

    if (strcmp ("GNU C++", lang_hooks.name) == 0 &&
        DECL_NAME (decl) != NULL)
      {
        /* The gcc/cp decl_printable_name hook doesn't do as good a job as
           the libiberty demangler.  */
        declname = cplus_demangle (IDENTIFIER_POINTER (DECL_NAME (decl)),
                                   DMGL_AUTO | DMGL_VERBOSE);
      }

    if (declname == NULL)
      declname = lang_hooks.decl_printable_name (decl, 3);

    if (declname == NULL)
      declname = "<unnamed variable>";

    pp_string (buf, declname);
  }

  /* Return the lot as a new STRING_CST.  */
  buf_contents = pp_base_formatted_text (buf);
  result = mf_build_string (buf_contents);
  pp_clear_output_area (buf);

  return result;
}


/* And another friend, for producing a simpler message.  */

static tree
mf_file_function_line_tree (location_t location)
{
  expanded_location xloc = expand_location (location);
  const char *file = NULL, *colon, *line, *op, *name, *cp;
  char linecolbuf[30]; /* Enough for two decimal numbers plus a colon.  */
  char *string;
  tree result;

  /* Add FILENAME[:LINENUMBER[:COLUMNNUMBER]].  */
  file = xloc.file;
  if (file == NULL && current_function_decl != NULL_TREE)
    file = DECL_SOURCE_FILE (current_function_decl);
  if (file == NULL)
    file = "<unknown file>";

  if (xloc.line > 0)
    {
#ifdef USE_MAPPED_LOCATION
      if (xloc.column > 0)
        sprintf (linecolbuf, "%d:%d", xloc.line, xloc.column);
      else
#endif
        sprintf (linecolbuf, "%d", xloc.line);
      colon = ":";
      line = linecolbuf;
    }
  else
    colon = line = "";

  /* Add (FUNCTION).  */
  name = lang_hooks.decl_printable_name (current_function_decl, 1);
  if (name)
    {
      op = " (";
      cp = ")";
    }
  else
    op = name = cp = "";

  string = concat (file, colon, line, op, name, cp, NULL);
  result = mf_build_string (string);
  free (string);

  return result;
}


/* global tree nodes */

/* Global tree objects for global variables and functions exported by
   mudflap runtime library.  mf_init_extern_trees must be called
   before using these.  */

/* uintptr_t (usually "unsigned long") */
static GTY (()) tree mf_uintptr_type;

/* struct __mf_cache { uintptr_t low; uintptr_t high; }; */
static GTY (()) tree mf_cache_struct_type;

/* struct __mf_cache * const */
static GTY (()) tree mf_cache_structptr_type;

/* extern struct __mf_cache __mf_lookup_cache []; */
static GTY (()) tree mf_cache_array_decl;

/* extern unsigned char __mf_lc_shift; */
static GTY (()) tree mf_cache_shift_decl;

/* extern uintptr_t __mf_lc_mask; */
static GTY (()) tree mf_cache_mask_decl;

/* Their function-scope local shadows, used in single-threaded mode only.  */

/* auto const unsigned char __mf_lc_shift_l; */
static GTY (()) tree mf_cache_shift_decl_l;

/* auto const uintptr_t __mf_lc_mask_l; */
static GTY (()) tree mf_cache_mask_decl_l;

/* extern void __mf_check (void *ptr, size_t sz, int type, const char *); */
static GTY (()) tree mf_check_fndecl;

/* extern void __mf_register (void *ptr, size_t sz, int type, const char *); */
static GTY (()) tree mf_register_fndecl;

/* extern void __mf_unregister (void *ptr, size_t sz, int type); */
static GTY (()) tree mf_unregister_fndecl;

/* extern void __mf_init (); */
static GTY (()) tree mf_init_fndecl;

/* extern int __mf_set_options (const char*); */
static GTY (()) tree mf_set_options_fndecl;


/* Helper for mudflap_init: construct a decl with the given category,
   name, and type, mark it an external reference, and pushdecl it.  */
static inline tree
mf_make_builtin (enum tree_code category, const char *name, tree type)
{
  tree decl = mf_mark (build_decl (category, get_identifier (name), type));
  TREE_PUBLIC (decl) = 1;
  DECL_EXTERNAL (decl) = 1;
  lang_hooks.decls.pushdecl (decl);
  return decl;
}

/* Helper for mudflap_init: construct a tree corresponding to the type
     struct __mf_cache { uintptr_t low; uintptr_t high; };
     where uintptr_t is the FIELD_TYPE argument.  */
static inline tree
mf_make_mf_cache_struct_type (tree field_type)
{
  /* There is, abominably, no language-independent way to construct a
     RECORD_TYPE.  So we have to call the basic type construction
     primitives by hand.  */
  tree fieldlo = build_decl (FIELD_DECL, get_identifier ("low"), field_type);
  tree fieldhi = build_decl (FIELD_DECL, get_identifier ("high"), field_type);

  tree struct_type = make_node (RECORD_TYPE);
  DECL_CONTEXT (fieldlo) = struct_type;
  DECL_CONTEXT (fieldhi) = struct_type;
  TREE_CHAIN (fieldlo) = fieldhi;
  TYPE_FIELDS (struct_type) = fieldlo;
  TYPE_NAME (struct_type) = get_identifier ("__mf_cache");
  layout_type (struct_type);

  return struct_type;
}

#define build_function_type_0(rtype)            \
  build_function_type (rtype, void_list_node)
#define build_function_type_1(rtype, arg1)                 \
  build_function_type (rtype, tree_cons (0, arg1, void_list_node))
#define build_function_type_3(rtype, arg1, arg2, arg3)                  \
  build_function_type (rtype, tree_cons (0, arg1, tree_cons (0, arg2,   \
                                                             tree_cons (0, arg3, void_list_node))))
#define build_function_type_4(rtype, arg1, arg2, arg3, arg4)            \
  build_function_type (rtype, tree_cons (0, arg1, tree_cons (0, arg2,   \
                                                             tree_cons (0, arg3, tree_cons (0, arg4, \
                                                                                            void_list_node)))))

/* Initialize the global tree nodes that correspond to mf-runtime.h
   declarations.  */
void
mudflap_init (void)
{
  static bool done = false;
  tree mf_const_string_type;
  tree mf_cache_array_type;
  tree mf_check_register_fntype;
  tree mf_unregister_fntype;
  tree mf_init_fntype;
  tree mf_set_options_fntype;

  if (done)
    return;
  done = true;

  mf_uintptr_type = lang_hooks.types.type_for_mode (ptr_mode,
                                                    /*unsignedp=*/true);
  mf_const_string_type
    = build_pointer_type (build_qualified_type
                          (char_type_node, TYPE_QUAL_CONST));

  mf_cache_struct_type = mf_make_mf_cache_struct_type (mf_uintptr_type);
  mf_cache_structptr_type = build_pointer_type (mf_cache_struct_type);
  mf_cache_array_type = build_array_type (mf_cache_struct_type, 0);
  mf_check_register_fntype =
    build_function_type_4 (void_type_node, ptr_type_node, size_type_node,
                           integer_type_node, mf_const_string_type);
  mf_unregister_fntype =
    build_function_type_3 (void_type_node, ptr_type_node, size_type_node,
                           integer_type_node);
  mf_init_fntype =
    build_function_type_0 (void_type_node);
  mf_set_options_fntype =
    build_function_type_1 (integer_type_node, mf_const_string_type);

  mf_cache_array_decl = mf_make_builtin (VAR_DECL, "__mf_lookup_cache",
                                         mf_cache_array_type);
  mf_cache_shift_decl = mf_make_builtin (VAR_DECL, "__mf_lc_shift",
                                         unsigned_char_type_node);
  mf_cache_mask_decl = mf_make_builtin (VAR_DECL, "__mf_lc_mask",
                                        mf_uintptr_type);
  mf_check_fndecl = mf_make_builtin (FUNCTION_DECL, "__mf_check",
                                     mf_check_register_fntype);
  mf_register_fndecl = mf_make_builtin (FUNCTION_DECL, "__mf_register",
                                        mf_check_register_fntype);
  mf_unregister_fndecl = mf_make_builtin (FUNCTION_DECL, "__mf_unregister",
                                          mf_unregister_fntype);
  mf_init_fndecl = mf_make_builtin (FUNCTION_DECL, "__mf_init",
                                    mf_init_fntype);
  mf_set_options_fndecl = mf_make_builtin (FUNCTION_DECL, "__mf_set_options",
                                           mf_set_options_fntype);
}
#undef build_function_type_4
#undef build_function_type_3
#undef build_function_type_1
#undef build_function_type_0


/* ------------------------------------------------------------------------ */
/* Memory reference transforms. Perform the mudflap indirection-related
   tree transforms on the current function.

   This is the second part of the mudflap instrumentation.  It works on
   low-level GIMPLE using the CFG, because we want to run this pass after
   tree optimizations have been performed, but we have to preserve the CFG
   for expansion from trees to RTL.  */

static void
execute_mudflap_function_ops (void)
{
  /* Don't instrument functions such as the synthetic constructor
     built during mudflap_finish_file.  */
  if (mf_marked_p (current_function_decl) ||
      DECL_ARTIFICIAL (current_function_decl))
    return;

  push_gimplify_context ();

  /* In multithreaded mode, don't cache the lookup cache parameters.  */
  if (! flag_mudflap_threads)
    mf_decl_cache_locals ();

  mf_xform_derefs ();

  if (! flag_mudflap_threads)
    mf_decl_clear_locals ();

  pop_gimplify_context (NULL);
}

/* Create and initialize local shadow variables for the lookup cache
   globals.  Put their decls in the *_l globals for use by
   mf_build_check_statement_for.  */

static void
mf_decl_cache_locals (void)
{
  tree t, shift_init_stmts, mask_init_stmts;
  tree_stmt_iterator tsi;

  /* Build the cache vars.  */
  mf_cache_shift_decl_l
    = mf_mark (create_tmp_var (TREE_TYPE (mf_cache_shift_decl),
                               "__mf_lookup_shift_l"));

  mf_cache_mask_decl_l
    = mf_mark (create_tmp_var (TREE_TYPE (mf_cache_mask_decl),
                               "__mf_lookup_mask_l"));

  /* Build initialization nodes for the cache vars.  We just load the
     globals into the cache variables.  */
  t = build (MODIFY_EXPR, TREE_TYPE (mf_cache_shift_decl_l),
             mf_cache_shift_decl_l, mf_cache_shift_decl);
  SET_EXPR_LOCATION (t, DECL_SOURCE_LOCATION (current_function_decl));
  gimplify_to_stmt_list (&t);
  shift_init_stmts = t;

  t = build (MODIFY_EXPR, TREE_TYPE (mf_cache_mask_decl_l),
             mf_cache_mask_decl_l, mf_cache_mask_decl);
  SET_EXPR_LOCATION (t, DECL_SOURCE_LOCATION (current_function_decl));
  gimplify_to_stmt_list (&t);
  mask_init_stmts = t;

  /* Anticipating multiple entry points, we insert the cache vars
     initializers in each successor of the ENTRY_BLOCK_PTR.  */
  for (tsi = tsi_start (shift_init_stmts);
       ! tsi_end_p (tsi);
       tsi_next (&tsi))
    insert_edge_copies (tsi_stmt (tsi), ENTRY_BLOCK_PTR);

  for (tsi = tsi_start (mask_init_stmts);
       ! tsi_end_p (tsi);
       tsi_next (&tsi))
    insert_edge_copies (tsi_stmt (tsi), ENTRY_BLOCK_PTR);
  bsi_commit_edge_inserts (NULL);
}


static void
mf_decl_clear_locals (void)
{
  /* Unset local shadows.  */
  mf_cache_shift_decl_l = NULL_TREE;
  mf_cache_mask_decl_l = NULL_TREE;
}

static void
mf_build_check_statement_for (tree base, tree addr, tree limit,
                              block_stmt_iterator *instr_bsi,
                              location_t *locus, tree dirflag)
{
  tree_stmt_iterator head, tsi;
  tree ptrtype = TREE_TYPE (addr);
  block_stmt_iterator bsi;
  basic_block cond_bb, then_bb, join_bb;
  edge e;
  tree cond, t, u, v, l1, l2;
  tree mf_value;
  tree mf_base;
  tree mf_elem;
  tree mf_limit;

  /* We first need to split the current basic block, and start altering
     the CFG.  This allows us to insert the statements we're about to
     construct into the right basic blocks.  The label l1 is the label
     of the block for the THEN clause of the conditional jump we're
     about to construct, and l2 is the ELSE clause, which is just the
     continuation of the old statement stream.  */
  l1 = create_artificial_label ();
  l2 = create_artificial_label ();
  cond_bb = bb_for_stmt (bsi_stmt (*instr_bsi));
  bsi = *instr_bsi;
  bsi_prev (&bsi);
  if (! bsi_end_p (bsi))
    {
      /* We're processing a statement in the middle of the block, so
         we need to split the block.  This creates a new block and a new
         fallthrough edge.  */
      e = split_block (cond_bb, bsi_stmt (bsi));
      cond_bb = e->src;
      join_bb = e->dest;
    }
  else
    {
      /* We're processing the first statement in the block, so we need
         to split the incoming edge.  This also creates a new block
         and a new fallthrough edge.  */
      join_bb = cond_bb;
      cond_bb = split_edge (find_edge (join_bb->prev_bb, join_bb));
    }
  
  /* A recap at this point: join_bb is the basic block at whose head
     is the gimple statement for which this check expression is being
     built.  cond_bb is the (possibly new, synthetic) basic block the
     end of which will contain the cache-lookup code, and a
     conditional that jumps to the cache-miss code or, much more
     likely, over to join_bb.  */

  /* Create the bb that contains the cache-miss fallback block (mf_check).  */
  then_bb = create_empty_bb (cond_bb);
  make_edge (cond_bb, then_bb, EDGE_TRUE_VALUE);
  make_single_succ_edge (then_bb, join_bb, EDGE_FALLTHRU);

  /* We expect that the conditional jump we will construct will not
     be taken very often as it basically is an exception condition.  */
  predict_edge_def (EDGE_PRED (then_bb, 0), PRED_MUDFLAP, NOT_TAKEN);

  /* Mark the pseudo-fallthrough edge from cond_bb to join_bb.  */
  e = find_edge (cond_bb, join_bb);
  e->flags = EDGE_FALSE_VALUE;
  predict_edge_def (e, PRED_MUDFLAP, TAKEN);

  /* Update dominance info.  Note that bb_join's data was
     updated by split_block.  */
  if (dom_computed[CDI_DOMINATORS] >= DOM_CONS_OK)
    {
      set_immediate_dominator (CDI_DOMINATORS, then_bb, cond_bb);
      set_immediate_dominator (CDI_DOMINATORS, join_bb, cond_bb);
    }

  /* Build our local variables.  */
  mf_value = create_tmp_var (ptrtype, "__mf_value");
  mf_elem = create_tmp_var (mf_cache_structptr_type, "__mf_elem");
  mf_base = create_tmp_var (mf_uintptr_type, "__mf_base");
  mf_limit = create_tmp_var (mf_uintptr_type, "__mf_limit");

  /* Build: __mf_value = <address expression>.  */
  t = build (MODIFY_EXPR, void_type_node, mf_value, unshare_expr (addr));
  SET_EXPR_LOCUS (t, locus);
  gimplify_to_stmt_list (&t);
  head = tsi_start (t);
  tsi = tsi_last (t);

  /* Build: __mf_base = (uintptr_t) <base address expression>.  */
  t = build (MODIFY_EXPR, void_type_node, mf_base,
             convert (mf_uintptr_type, unshare_expr (base)));
  SET_EXPR_LOCUS (t, locus);
  gimplify_to_stmt_list (&t);
  tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

  /* Build: __mf_limit = (uintptr_t) <limit address expression>.  */
  t = build (MODIFY_EXPR, void_type_node, mf_limit,
             convert (mf_uintptr_type, unshare_expr (limit)));
  SET_EXPR_LOCUS (t, locus);
  gimplify_to_stmt_list (&t);
  tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

  /* Build: __mf_elem = &__mf_lookup_cache [(__mf_base >> __mf_shift)
                                            & __mf_mask].  */
  t = build (RSHIFT_EXPR, mf_uintptr_type, mf_base,
             (flag_mudflap_threads ? mf_cache_shift_decl : mf_cache_shift_decl_l));
  t = build (BIT_AND_EXPR, mf_uintptr_type, t,
             (flag_mudflap_threads ? mf_cache_mask_decl : mf_cache_mask_decl_l));
  t = build (ARRAY_REF,
             TREE_TYPE (TREE_TYPE (mf_cache_array_decl)),
             mf_cache_array_decl, t, NULL_TREE, NULL_TREE);
  t = build1 (ADDR_EXPR, mf_cache_structptr_type, t);
  t = build (MODIFY_EXPR, void_type_node, mf_elem, t);
  SET_EXPR_LOCUS (t, locus);
  gimplify_to_stmt_list (&t);
  tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

  /* Quick validity check.

     if (__mf_elem->low > __mf_base
         || (__mf_elem_high < __mf_limit))
        {
          __mf_check ();
          ... and only if single-threaded:
          __mf_lookup_shift_1 = f...;
          __mf_lookup_mask_l = ...;
        }

     It is expected that this body of code is rarely executed so we mark
     the edge to the THEN clause of the conditional jump as unlikely.  */

  /* Construct t <-- '__mf_elem->low  > __mf_base'.  */
  t = build (COMPONENT_REF, mf_uintptr_type,
             build1 (INDIRECT_REF, mf_cache_struct_type, mf_elem),
             TYPE_FIELDS (mf_cache_struct_type), NULL_TREE);
  t = build (GT_EXPR, boolean_type_node, t, mf_base);

  /* Construct '__mf_elem->high < __mf_limit'.

     First build:
        1) u <--  '__mf_elem->high'
        2) v <--  '__mf_limit'.

     Then build 'u <-- (u < v).  */

  u = build (COMPONENT_REF, mf_uintptr_type,
             build1 (INDIRECT_REF, mf_cache_struct_type, mf_elem),
             TREE_CHAIN (TYPE_FIELDS (mf_cache_struct_type)), NULL_TREE);

  v = mf_limit;

  u = build (LT_EXPR, boolean_type_node, u, v);

  /* Build the composed conditional: t <-- 't || u'.  Then store the
     result of the evaluation of 't' in a temporary variable which we
     can use as the condition for the conditional jump.  */
  t = build (TRUTH_OR_EXPR, boolean_type_node, t, u);
  cond = create_tmp_var (boolean_type_node, "__mf_unlikely_cond");
  t = build (MODIFY_EXPR, boolean_type_node, cond, t);
  gimplify_to_stmt_list (&t);
  tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

  /* Build the conditional jump.  'cond' is just a temporary so we can
     simply build a void COND_EXPR.  We do need labels in both arms though.  */
  t = build (COND_EXPR, void_type_node, cond,
             build (GOTO_EXPR, void_type_node, tree_block_label (then_bb)),
             build (GOTO_EXPR, void_type_node, tree_block_label (join_bb)));
  SET_EXPR_LOCUS (t, locus);
  tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

  /* At this point, after so much hard work, we have only constructed
     the conditional jump,

     if (__mf_elem->low > __mf_base
         || (__mf_elem_high < __mf_limit))

     The lowered GIMPLE tree representing this code is in the statement
     list starting at 'head'.

     We can insert this now in the current basic block, i.e. the one that
     the statement we're instrumenting was originally in.  */
  bsi = bsi_last (cond_bb);
  for (tsi = head; ! tsi_end_p (tsi); tsi_next (&tsi))
    bsi_insert_after (&bsi, tsi_stmt (tsi), BSI_CONTINUE_LINKING);

  /*  Now build up the body of the cache-miss handling:

     __mf_check();
     refresh *_l vars.

     This is the body of the conditional.  */
  
  u = tree_cons (NULL_TREE,
                 mf_file_function_line_tree (locus == NULL ? UNKNOWN_LOCATION
                                             : *locus),
                 NULL_TREE);
  u = tree_cons (NULL_TREE, dirflag, u);
  /* NB: we pass the overall [base..limit] range to mf_check,
     not the [mf_value..mf_value+size-1] range.  */
  u = tree_cons (NULL_TREE, 
                 fold (build (PLUS_EXPR, integer_type_node,
                              fold (build (MINUS_EXPR, mf_uintptr_type, mf_limit, mf_base)),
                              integer_one_node)),
                 u);
  u = tree_cons (NULL_TREE, mf_base, u);
  t = build_function_call_expr (mf_check_fndecl, u);
  gimplify_to_stmt_list (&t);
  head = tsi_start (t);
  tsi = tsi_last (t);

  if (! flag_mudflap_threads)
    {
      t = build (MODIFY_EXPR, void_type_node,
                 mf_cache_shift_decl_l, mf_cache_shift_decl);
      tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);

      t = build (MODIFY_EXPR, void_type_node,
                 mf_cache_mask_decl_l, mf_cache_mask_decl);
      tsi_link_after (&tsi, t, TSI_CONTINUE_LINKING);
    }

  /* Insert the check code in the THEN block.  */
  bsi = bsi_start (then_bb);
  for (tsi = head; ! tsi_end_p (tsi); tsi_next (&tsi))
    bsi_insert_after (&bsi, tsi_stmt (tsi), BSI_CONTINUE_LINKING);

  *instr_bsi = bsi_start (join_bb);
  bsi_next (instr_bsi);
}

static void
mf_xform_derefs_1 (block_stmt_iterator *iter, tree *tp,
                   location_t *locus, tree dirflag)
{
  tree type, ptr_type, addr, base, size, limit, t;

  /* Don't instrument read operations.  */
  if (dirflag == integer_zero_node && flag_mudflap_ignore_reads)
    return;

  /* Don't instrument marked nodes.  */
  if (mf_marked_p (*tp))
    return;

  t = *tp;
  type = TREE_TYPE (t);
  size = TYPE_SIZE_UNIT (type);

  switch (TREE_CODE (t))
    {
    case ARRAY_REF:
      {
	/* Omit checking if we can statically determine that the access is
	   valid.  For non-addressable local arrays this is not optional,
	   since we won't have called __mf_register for the object.  */
	/* APPLE LOCAL begin lno */
	tree tt, op0, op1;

	tt = t;
	op0 = TREE_OPERAND (tt, 0);
	op1 = TREE_OPERAND (tt, 1);
	while (in_array_bounds_p (tt))
	  {
	    /* If we're looking at a non-external VAR_DECL, then the 
	       access must be ok.  */
	    if (TREE_CODE (op0) == VAR_DECL && !DECL_EXTERNAL (op0))
	      return;

	    /* Only continue if we're still looking at an array.  */
	    if (TREE_CODE (op0) != ARRAY_REF)
	      break;

	    tt = op0;
	    op1 = TREE_OPERAND (tt, 1);
	    op0 = TREE_OPERAND (tt, 0);
	  }
        /* APPLE LOCAL end lno */
      
        /* If we got here, we couldn't statically the check.  */
        ptr_type = build_pointer_type (type);
        addr = build1 (ADDR_EXPR, ptr_type, t);
        base = build1 (ADDR_EXPR, ptr_type, op0);
        limit = fold (build (MINUS_EXPR, mf_uintptr_type,
                             fold (build2 (PLUS_EXPR, mf_uintptr_type, addr, size)),
                             integer_one_node));
      }
      break;

    case INDIRECT_REF:
      addr = TREE_OPERAND (t, 0);
      ptr_type = TREE_TYPE (addr);
      base = addr;
      limit = fold (build (MINUS_EXPR, ptr_type_node,
                           fold (build (PLUS_EXPR, ptr_type_node, base, size)),
                           integer_one_node));
      break;

    case ARRAY_RANGE_REF:
      warning ("mudflap checking not yet implemented for ARRAY_RANGE_REF");
      return;

    case COMPONENT_REF:
      {
        tree field;

        /* If we're not dereferencing something, then the access
           must be ok.  */
        if (TREE_CODE (TREE_OPERAND (t, 0)) != INDIRECT_REF)
          return;

        field = TREE_OPERAND (t, 1);

        /* If we're looking at a bit field, then we can't take its address
           with ADDR_EXPR -- lang_hooks.mark_addressable will error.  Do
           things the hard way with PLUS.  */
        if (DECL_BIT_FIELD_TYPE (field))
          {
            if (TREE_CODE (DECL_SIZE_UNIT (field)) == INTEGER_CST)
              size = DECL_SIZE_UNIT (field);

            addr = TREE_OPERAND (TREE_OPERAND (t, 0), 0);
            addr = fold_convert (ptr_type_node, addr);
            addr = fold (build (PLUS_EXPR, ptr_type_node,
                                addr, fold_convert (ptr_type_node,
                                                    byte_position (field))));
          }
        else
          {
            ptr_type = build_pointer_type (type);
            addr = build1 (ADDR_EXPR, ptr_type, t);
          }

        /* XXXXXX */
        base = addr;
        limit = fold (build (MINUS_EXPR, ptr_type_node,
                             fold (build (PLUS_EXPR, ptr_type_node, base, size)),
                             integer_one_node));
      }
      break;

    case BIT_FIELD_REF:
      {
        tree ofs, rem, bpu;

        /* If we're not dereferencing something, then the access
           must be ok.  */
        if (TREE_CODE (TREE_OPERAND (t, 0)) != INDIRECT_REF)
          return;

        bpu = bitsize_int (BITS_PER_UNIT);
        ofs = convert (bitsizetype, TREE_OPERAND (t, 2));
        rem = size_binop (TRUNC_MOD_EXPR, ofs, bpu);
        ofs = size_binop (TRUNC_DIV_EXPR, ofs, bpu);

        size = convert (bitsizetype, TREE_OPERAND (t, 1));
        size = size_binop (PLUS_EXPR, size, rem);
        size = size_binop (CEIL_DIV_EXPR, size, bpu);
        size = convert (sizetype, size);

        addr = TREE_OPERAND (TREE_OPERAND (t, 0), 0);
        addr = convert (ptr_type_node, addr);
        addr = fold (build (PLUS_EXPR, ptr_type_node, addr, ofs));

        base = addr;
        limit = fold (build (MINUS_EXPR, ptr_type_node,
                             fold (build (PLUS_EXPR, ptr_type_node, base, size)),
                             integer_one_node));
      }
      break;

    default:
      return;
    }

  mf_build_check_statement_for (base, addr, limit, iter, locus, dirflag);
}

static void
mf_xform_derefs (void)
{
  basic_block bb, next;
  block_stmt_iterator i;
  int saved_last_basic_block = last_basic_block;

  bb = ENTRY_BLOCK_PTR ->next_bb;
  do
    {
      next = bb->next_bb;
      for (i = bsi_start (bb); !bsi_end_p (i); bsi_next (&i))
        {
          tree s = bsi_stmt (i);

          /* Only a few GIMPLE statements can reference memory.  */
          switch (TREE_CODE (s))
            {
            case MODIFY_EXPR:
              mf_xform_derefs_1 (&i, &TREE_OPERAND (s, 0), EXPR_LOCUS (s),
                                 integer_one_node);
              mf_xform_derefs_1 (&i, &TREE_OPERAND (s, 1), EXPR_LOCUS (s),
                                 integer_zero_node);
              break;

            case RETURN_EXPR:
              if (TREE_OPERAND (s, 0) != NULL_TREE)
                {
                  if (TREE_CODE (TREE_OPERAND (s, 0)) == MODIFY_EXPR)
                    mf_xform_derefs_1 (&i, &TREE_OPERAND (TREE_OPERAND (s, 0), 1),
                                       EXPR_LOCUS (s), integer_zero_node);
                  else
                    mf_xform_derefs_1 (&i, &TREE_OPERAND (s, 0), EXPR_LOCUS (s),
                                       integer_zero_node);
                }
              break;

            default:
              ;
            }
        }
      bb = next;
    }
  while (bb && bb->index <= saved_last_basic_block);
}

/* ------------------------------------------------------------------------ */
/* ADDR_EXPR transforms.  Perform the declaration-related mudflap tree
   transforms on the current function.

   This is the first part of the mudflap instrumentation.  It works on
   high-level GIMPLE because after lowering, all variables are moved out
   of their BIND_EXPR binding context, and we lose liveness information
   for the declarations we wish to instrument.  */

static void
execute_mudflap_function_decls (void)
{
  /* Don't instrument functions such as the synthetic constructor
     built during mudflap_finish_file.  */
  if (mf_marked_p (current_function_decl) ||
      DECL_ARTIFICIAL (current_function_decl))
    return;

  push_gimplify_context ();

  mf_xform_decls (DECL_SAVED_TREE (current_function_decl),
                  DECL_ARGUMENTS (current_function_decl));

  pop_gimplify_context (NULL);
}

/* This struct is passed between mf_xform_decls to store state needed
   during the traversal searching for objects that have their
   addresses taken.  */
struct mf_xform_decls_data
{
  tree param_decls;
};


/* Synthesize a CALL_EXPR and a TRY_FINALLY_EXPR, for this chain of
   _DECLs if appropriate.  Arrange to call the __mf_register function
   now, and the __mf_unregister function later for each.  */
static void
mx_register_decls (tree decl, tree *stmt_list)
{
  tree finally_stmts = NULL_TREE;
  tree_stmt_iterator initially_stmts = tsi_start (*stmt_list);

  while (decl != NULL_TREE)
    {
      /* Eligible decl?  */
      if ((TREE_CODE (decl) == VAR_DECL || TREE_CODE (decl) == PARM_DECL)
          /* It must be a non-external, automatic variable.  */
          && ! DECL_EXTERNAL (decl)
          && ! TREE_STATIC (decl)
          /* The decl must have its address taken.  */
          && TREE_ADDRESSABLE (decl)
          /* The type of the variable must be complete.  */
          && COMPLETE_OR_VOID_TYPE_P (TREE_TYPE (decl))
	  /* The decl hasn't been decomposed somehow.  */
	  && DECL_VALUE_EXPR (decl) == NULL
          /* Don't process the same decl twice.  */
          && ! mf_marked_p (decl))
        {
          tree size = NULL_TREE, variable_name;
          tree unregister_fncall, unregister_fncall_params;
          tree register_fncall, register_fncall_params;

	  size = convert (size_type_node, TYPE_SIZE_UNIT (TREE_TYPE (decl)));

          /* (& VARIABLE, sizeof (VARIABLE), __MF_TYPE_STACK) */
          unregister_fncall_params =
            tree_cons (NULL_TREE,
                       convert (ptr_type_node,
                                mf_mark (build1 (ADDR_EXPR,
                                                 build_pointer_type (TREE_TYPE (decl)),
                                                 decl))),
                       tree_cons (NULL_TREE, 
                                  size,
                                  tree_cons (NULL_TREE,
					     /* __MF_TYPE_STACK */
                                             build_int_cst (NULL_TREE, 3),
                                             NULL_TREE)));
          /* __mf_unregister (...) */
          unregister_fncall = build_function_call_expr (mf_unregister_fndecl,
                                                        unregister_fncall_params);

          /* (& VARIABLE, sizeof (VARIABLE), __MF_TYPE_STACK, "name") */
          variable_name = mf_varname_tree (decl);
          register_fncall_params =
            tree_cons (NULL_TREE,
                   convert (ptr_type_node,
                            mf_mark (build1 (ADDR_EXPR,
                                             build_pointer_type (TREE_TYPE (decl)),
                                             decl))),
                       tree_cons (NULL_TREE,
                                  size,
                                  tree_cons (NULL_TREE,
					     /* __MF_TYPE_STACK */
                                             build_int_cst (NULL_TREE, 3),
                                             tree_cons (NULL_TREE,
                                                        variable_name,
                                                        NULL_TREE))));

          /* __mf_register (...) */
          register_fncall = build_function_call_expr (mf_register_fndecl,
                                                      register_fncall_params);

          /* Accumulate the two calls.  */
          /* ??? Set EXPR_LOCATION.  */
          gimplify_stmt (&register_fncall);
          gimplify_stmt (&unregister_fncall);

          /* Add the __mf_register call at the current appending point.  */
          if (tsi_end_p (initially_stmts))
            internal_error ("mudflap ran off end of BIND_EXPR body");
          tsi_link_before (&initially_stmts, register_fncall, TSI_SAME_STMT);

          /* Accumulate the FINALLY piece.  */
          append_to_statement_list (unregister_fncall, &finally_stmts);

          mf_mark (decl);
        }

      decl = TREE_CHAIN (decl);
    }

  /* Actually, (initially_stmts!=NULL) <=> (finally_stmts!=NULL) */
  if (finally_stmts != NULL_TREE)
    {
      tree t = build (TRY_FINALLY_EXPR, void_type_node,
                      *stmt_list, finally_stmts);
      *stmt_list = NULL;
      append_to_statement_list (t, stmt_list);
    }
}


/* Process every variable mentioned in BIND_EXPRs.  */
static tree
mx_xfn_xform_decls (tree *t, int *continue_p, void *data)
{
  struct mf_xform_decls_data* d = (struct mf_xform_decls_data*) data;

  if (*t == NULL_TREE || *t == error_mark_node)
    {
      *continue_p = 0;
      return NULL_TREE;
    }

  *continue_p = 1;

  switch (TREE_CODE (*t))
    {
    case BIND_EXPR:
      {
        /* Process function parameters now (but only once).  */
        mx_register_decls (d->param_decls, &BIND_EXPR_BODY (*t));
        d->param_decls = NULL_TREE;

        mx_register_decls (BIND_EXPR_VARS (*t), &BIND_EXPR_BODY (*t));
      }
      break;

    default:
      break;
    }

  return NULL;
}

/* Perform the object lifetime tracking mudflap transform on the given function
   tree.  The tree is mutated in place, with possibly copied subtree nodes.

   For every auto variable declared, if its address is ever taken
   within the function, then supply its lifetime to the mudflap
   runtime with the __mf_register and __mf_unregister calls.
*/

static void
mf_xform_decls (tree fnbody, tree fnparams)
{
  struct mf_xform_decls_data d;
  d.param_decls = fnparams;
  walk_tree_without_duplicates (&fnbody, mx_xfn_xform_decls, &d);
}


/* ------------------------------------------------------------------------ */
/* Externally visible mudflap functions.  */


/* Mark and return the given tree node to prevent further mudflap
   transforms.  */
static GTY ((param_is (union tree_node))) htab_t marked_trees = NULL;

tree
mf_mark (tree t)
{
  void **slot;

  if (marked_trees == NULL)
    marked_trees = htab_create_ggc (31, htab_hash_pointer, htab_eq_pointer, NULL);

  slot = htab_find_slot (marked_trees, t, INSERT);
  *slot = t;
  return t;
}

int
mf_marked_p (tree t)
{
  void *entry;

  if (marked_trees == NULL)
    return 0;

  entry = htab_find (marked_trees, t);
  return (entry != NULL);
}

/* Remember given node as a static of some kind: global data,
   function-scope static, or an anonymous constant.  Its assembler
   label is given.  */

/* A list of globals whose incomplete declarations we encountered.
   Instead of emitting the __mf_register call for them here, it's
   delayed until program finish time.  If they're still incomplete by
   then, warnings are emitted.  */

static GTY (()) varray_type deferred_static_decls;

/* A list of statements for calling __mf_register() at startup time.  */
static GTY (()) tree enqueued_call_stmt_chain;

static void
mudflap_register_call (tree obj, tree object_size, tree varname)
{
  tree arg, args, call_stmt;

  args = tree_cons (NULL_TREE, varname, NULL_TREE);

  arg = build_int_cst (NULL_TREE, 4); /* __MF_TYPE_STATIC */
  args = tree_cons (NULL_TREE, arg, args);

  arg = convert (size_type_node, object_size);
  args = tree_cons (NULL_TREE, arg, args);

  arg = build1 (ADDR_EXPR, build_pointer_type (TREE_TYPE (obj)), obj);
  arg = convert (ptr_type_node, arg);
  args = tree_cons (NULL_TREE, arg, args);

  call_stmt = build_function_call_expr (mf_register_fndecl, args);

  append_to_statement_list (call_stmt, &enqueued_call_stmt_chain);
}

void
mudflap_enqueue_decl (tree obj)
{
  if (mf_marked_p (obj))
    return;

  /* We don't need to process variable decls that are internally
     generated extern.  If we did, we'd end up with warnings for them
     during mudflap_finish_file ().  That would confuse the user,
     since the text would refer to variables that don't show up in the
     user's source code.  */
  if (DECL_P (obj) && DECL_EXTERNAL (obj) && DECL_ARTIFICIAL (obj))
    return;

  if (COMPLETE_TYPE_P (TREE_TYPE (obj)))
    {
      tree object_size;

      mf_mark (obj);

      object_size = size_in_bytes (TREE_TYPE (obj));

      if (dump_file)
        {
          fprintf (dump_file, "enqueue_decl obj=`");
          print_generic_expr (dump_file, obj, dump_flags);
          fprintf (dump_file, "' size=");
          print_generic_expr (dump_file, object_size, dump_flags);
          fprintf (dump_file, "\n");
        }

      /* NB: the above condition doesn't require TREE_USED or
         TREE_ADDRESSABLE.  That's because this object may be a global
         only used from other compilation units.  XXX: Maybe static
         objects could require those attributes being set.  */

      mudflap_register_call (obj, object_size, mf_varname_tree (obj));
    }
  else
    {
      size_t i;

      if (! deferred_static_decls)
        VARRAY_TREE_INIT (deferred_static_decls, 10, "deferred static list");

      /* Ugh, linear search... */
      for (i = 0; i < VARRAY_ACTIVE_SIZE (deferred_static_decls); i++)
        if (VARRAY_TREE (deferred_static_decls, i) == obj)
          {
            warning ("mudflap cannot track lifetime of %qs",
                     IDENTIFIER_POINTER (DECL_NAME (obj)));
            return;
          }

      VARRAY_PUSH_TREE (deferred_static_decls, obj);
    }
}

void
mudflap_enqueue_constant (tree obj)
{
  tree object_size, varname;

  if (mf_marked_p (obj))
    return;

  if (TREE_CODE (obj) == STRING_CST)
    object_size = build_int_cst (NULL_TREE, TREE_STRING_LENGTH (obj));
  else
    object_size = size_in_bytes (TREE_TYPE (obj));

  if (dump_file)
    {
      fprintf (dump_file, "enqueue_constant obj=`");
      print_generic_expr (dump_file, obj, dump_flags);
      fprintf (dump_file, "' size=");
      print_generic_expr (dump_file, object_size, dump_flags);
      fprintf (dump_file, "\n");
    }

  if (TREE_CODE (obj) == STRING_CST)
    varname = mf_build_string ("string literal");
  else
    varname = mf_build_string ("constant");

  mudflap_register_call (obj, object_size, varname);
}


/* Emit any file-wide instrumentation.  */
void
mudflap_finish_file (void)
{
  tree ctor_statements = NULL_TREE;

  /* Try to give the deferred objects one final try.  */
  if (deferred_static_decls)
    {
      size_t i;

      for (i = 0; i < VARRAY_ACTIVE_SIZE (deferred_static_decls); i++)
        {
          tree obj = VARRAY_TREE (deferred_static_decls, i);

          /* Call enqueue_decl again on the same object it has previously
             put into the table.  (It won't modify the table this time, so
             infinite iteration is not a problem.)  */
          mudflap_enqueue_decl (obj);
        }

      VARRAY_CLEAR (deferred_static_decls);
    }

  /* Insert a call to __mf_init.  */
  {
    tree call2_stmt = build_function_call_expr (mf_init_fndecl, NULL_TREE);
    append_to_statement_list (call2_stmt, &ctor_statements);
  }
  
  /* If appropriate, call __mf_set_options to pass along read-ignore mode.  */
  if (flag_mudflap_ignore_reads)
    {
      tree arg = tree_cons (NULL_TREE, 
                            mf_build_string ("-ignore-reads"), NULL_TREE);
      tree call_stmt = build_function_call_expr (mf_set_options_fndecl, arg);
      append_to_statement_list (call_stmt, &ctor_statements);
    }

  /* Append all the enqueued registration calls.  */
  if (enqueued_call_stmt_chain)
    {
      append_to_statement_list (enqueued_call_stmt_chain, &ctor_statements);
      enqueued_call_stmt_chain = NULL_TREE;
    }

  cgraph_build_static_cdtor ('I', ctor_statements, 
                             MAX_RESERVED_INIT_PRIORITY-1);
}


static bool
gate_mudflap (void)
{
  return flag_mudflap != 0;
}

struct tree_opt_pass pass_mudflap_1 = 
{
  "mudflap1",                           /* name */
  gate_mudflap,                         /* gate */
  execute_mudflap_function_decls,       /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  0,                                    /* tv_id */
  PROP_gimple_any,                      /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_dump_func,                       /* todo_flags_finish */
  0					/* letter */
};

struct tree_opt_pass pass_mudflap_2 = 
{
  "mudflap2",                           /* name */
  gate_mudflap,                         /* gate */
  execute_mudflap_function_ops,         /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  0,                                    /* tv_id */
  PROP_gimple_leh,                      /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_verify_flow | TODO_verify_stmts
  | TODO_dump_func,                     /* todo_flags_finish */
  0					/* letter */
};

#include "gt-tree-mudflap.h"
