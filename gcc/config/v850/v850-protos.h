/* Prototypes for v850.c functions used in the md file & elsewhere.
   Copyright (C) 1999, 2000, 2002 Free Software Foundation, Inc.

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

/* Function prototypes that cannot exist in v850.h due to dependency
   complications.  */
#ifndef GCC_V850_PROTOS_H
#define GCC_V850_PROTOS_H

#define Mmode enum machine_mode

extern void   expand_prologue               (void);
extern void   expand_epilogue               (void);
extern void   sdata_section                 (void);
extern void   rosdata_section               (void);
extern void   sbss_section                  (void);
extern void   tdata_section                 (void);
extern void   zdata_section                 (void);
extern void   rozdata_section               (void);
extern void   zbss_section                  (void);
extern int    v850_handle_pragma            (int (*)(void), void (*)(int), char *);
extern void   override_options              (void);
extern int    compute_register_save_size    (long *);
extern int    compute_frame_size            (int, long *);
extern void   v850_init_expanders           (void);

#ifdef RTX_CODE
extern int    v850_output_addr_const_extra  (FILE *, rtx);
extern rtx    v850_return_addr              (int);
extern void   print_operand                 (FILE *, rtx, int );
extern void   print_operand_address         (FILE *, rtx);
extern const char *output_move_double       (rtx *);
extern const char *output_move_single       (rtx *);
extern void   notice_update_cc              (rtx, rtx);
extern char * construct_save_jarl           (rtx);
extern char * construct_restore_jr          (rtx);
#ifdef HAVE_MACHINE_MODES
extern int    reg_or_int9_operand           (rtx, Mmode);
extern int    reg_or_const_operand          (rtx, Mmode);
extern char * construct_dispose_instruction (rtx);
extern char * construct_prepare_instruction (rtx);
extern int    pattern_is_ok_for_prepare     (rtx, Mmode);
extern int    pattern_is_ok_for_dispose     (rtx, Mmode);
extern int    ep_memory_operand             (rtx, Mmode, int);
extern int    reg_or_0_operand              (rtx, Mmode);
extern int    reg_or_int5_operand           (rtx, Mmode);
extern int    call_address_operand          (rtx, Mmode);
extern int    movsi_source_operand          (rtx, Mmode);
extern int    power_of_two_operand          (rtx, Mmode);
extern int    not_power_of_two_operand      (rtx, Mmode);
extern int    special_symbolref_operand     (rtx, Mmode);
extern int    pattern_is_ok_for_prologue    (rtx, Mmode);
extern int    pattern_is_ok_for_epilogue    (rtx, Mmode);
extern int    register_is_ok_for_epilogue   (rtx, Mmode);
#ifdef TREE_CODE
extern rtx    function_arg                  (CUMULATIVE_ARGS *, Mmode, tree, int);
#endif
#endif
#endif /* TREE_CODE */

#ifdef TREE_CODE
extern int    v850_interrupt_function_p     (tree);
extern void   v850_output_aligned_bss       (FILE *, tree, const char *, int, int);
extern void   v850_output_common            (FILE *, tree, const char *, int, int);
extern void   v850_output_local             (FILE *, tree, const char *, int, int);
extern v850_data_area v850_get_data_area    (tree);
#ifdef HAVE_MACHINE_MODES
extern int    function_arg_partial_nregs    (CUMULATIVE_ARGS *, Mmode, tree, int);
#endif
#endif

extern void ghs_pragma_section		    (struct cpp_reader *);
extern void ghs_pragma_interrupt	    (struct cpp_reader *);
extern void ghs_pragma_starttda		    (struct cpp_reader *);
extern void ghs_pragma_startsda		    (struct cpp_reader *);
extern void ghs_pragma_startzda		    (struct cpp_reader *);
extern void ghs_pragma_endtda		    (struct cpp_reader *);
extern void ghs_pragma_endsda		    (struct cpp_reader *);
extern void ghs_pragma_endzda		    (struct cpp_reader *);

#undef  Mmode

#endif /* ! GCC_V850_PROTOS_H */
