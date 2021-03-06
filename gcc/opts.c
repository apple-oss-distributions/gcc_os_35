/* Command line option handling.
   Copyright (C) 2002, 2003, 2004 Free Software Foundation, Inc.
   Contributed by Neil Booth.

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
#include "system.h"
#include "intl.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "rtl.h"
#include "ggc.h"
#include "output.h"
#include "langhooks.h"
#include "opts.h"
#include "options.h"
#include "flags.h"
#include "toplev.h"
#include "params.h"
#include "diagnostic.h"
#include "tm_p.h"		/* For OPTIMIZATION_OPTIONS.  */
#include "insn-attr.h"		/* For INSN_SCHEDULING.  */
#include "target.h"

/* Value of the -G xx switch, and whether it was passed or not.  */
unsigned HOST_WIDE_INT g_switch_value;
bool g_switch_set;

/* True if we should exit after parsing options.  */
bool exit_after_options;

/* Print various extra warnings.  -W/-Wextra.  */
bool extra_warnings;

/* True to warn about any objects definitions whose size is larger
   than N bytes.  Also want about function definitions whose returned
   values are larger than N bytes, where N is `larger_than_size'.  */
bool warn_larger_than;
HOST_WIDE_INT larger_than_size;

/* Nonzero means warn about constructs which might not be
   strict-aliasing safe.  */
int warn_strict_aliasing;

/* Hack for cooperation between set_Wunused and set_Wextra.  */
static bool maybe_warn_unused_parameter;

/* Type(s) of debugging information we are producing (if any).  See
   flags.h for the definitions of the different possible types of
   debugging information.  */
enum debug_info_type write_symbols = NO_DEBUG;

/* Level of debugging information we are producing.  See flags.h for
   the definitions of the different possible levels.  */
enum debug_info_level debug_info_level = DINFO_LEVEL_NONE;

/* Nonzero means use GNU-only extensions in the generated symbolic
   debugging information.  Currently, this only has an effect when
   write_symbols is set to DBX_DEBUG, XCOFF_DEBUG, or DWARF_DEBUG.  */
bool use_gnu_debug_info_extensions;

/* The default visibility for all symbols (unless overridden) */
enum symbol_visibility default_visibility = VISIBILITY_DEFAULT;

/* Global visibility options.  */
struct visibility_flags visibility_options;

/* Columns of --help display.  */
static unsigned int columns = 80;

/* What to print when a switch has no documentation.  */
static const char undocumented_msg[] = N_("This switch lacks documentation");

/* Used for bookkeeping on whether user set these flags so
   -fprofile-use/-fprofile-generate does not use them.  */
static bool profile_arc_flag_set, flag_profile_values_set;
static bool flag_unroll_loops_set, flag_tracer_set;
static bool flag_value_profile_transformations_set;
bool flag_speculative_prefetching_set;
static bool flag_peel_loops_set, flag_branch_probabilities_set;

/* Input file names.  */
const char **in_fnames;
unsigned num_in_fnames;

static size_t find_opt (const char *, int);
static int common_handle_option (size_t scode, const char *arg, int value);
static void handle_param (const char *);
static void set_Wextra (int);
static unsigned int handle_option (const char **argv, unsigned int lang_mask);
static char *write_langs (unsigned int lang_mask);
static void complain_wrong_lang (const char *, const struct cl_option *,
				 unsigned int lang_mask);
static void handle_options (unsigned int, const char **, unsigned int);
static void wrap_help (const char *help, const char *item, unsigned int);
static void print_help (void);
static void print_param_help (void);
static void print_filtered_help (unsigned int flag);
static unsigned int print_switch (const char *text, unsigned int indent);
static void set_debug_level (enum debug_info_type type, int extended,
			     const char *arg);

/* Perform a binary search to find which option the command-line INPUT
   matches.  Returns its index in the option array, and N_OPTS
   (cl_options_count) on failure.

   This routine is quite subtle.  A normal binary search is not good
   enough because some options can be suffixed with an argument, and
   multiple sub-matches can occur, e.g. input of "-pedantic" matching
   the initial substring of "-pedantic-errors".

   A more complicated example is -gstabs.  It should match "-g" with
   an argument of "stabs".  Suppose, however, that the number and list
   of switches are such that the binary search tests "-gen-decls"
   before having tested "-g".  This doesn't match, and as "-gen-decls"
   is less than "-gstabs", it will become the lower bound of the
   binary search range, and "-g" will never be seen.  To resolve this
   issue, opts.sh makes "-gen-decls" point, via the back_chain member,
   to "-g" so that failed searches that end between "-gen-decls" and
   the lexicographically subsequent switch know to go back and see if
   "-g" causes a match (which it does in this example).

   This search is done in such a way that the longest match for the
   front end in question wins.  If there is no match for the current
   front end, the longest match for a different front end is returned
   (or N_OPTS if none) and the caller emits an error message.  */
static size_t
find_opt (const char *input, int lang_mask)
{
  size_t mn, mx, md, opt_len;
  size_t match_wrong_lang;
  int comp;

  mn = 0;
  mx = cl_options_count;

  /* Find mn such this lexicographical inequality holds:
     cl_options[mn] <= input < cl_options[mn + 1].  */
  while (mx - mn > 1)
    {
      md = (mn + mx) / 2;
      opt_len = cl_options[md].opt_len;
      comp = strncmp (input, cl_options[md].opt_text + 1, opt_len);

      if (comp < 0)
	mx = md;
      else
	mn = md;
    }

  /* This is the switch that is the best match but for a different
     front end, or cl_options_count if there is no match at all.  */
  match_wrong_lang = cl_options_count;

  /* Backtrace the chain of possible matches, returning the longest
     one, if any, that fits best.  With current GCC switches, this
     loop executes at most twice.  */
  do
    {
      const struct cl_option *opt = &cl_options[mn];

      /* Is this switch a prefix of the input?  */
      if (!strncmp (input, opt->opt_text + 1, opt->opt_len))
	{
	  /* If language is OK, and the match is exact or the switch
	     takes a joined argument, return it.  */
	  if ((opt->flags & lang_mask)
	      && (input[opt->opt_len] == '\0' || (opt->flags & CL_JOINED)))
	    return mn;

	  /* If we haven't remembered a prior match, remember this
	     one.  Any prior match is necessarily better.  */
	  if (match_wrong_lang == cl_options_count)
	    match_wrong_lang = mn;
	}

      /* Try the next possibility.  This is cl_options_count if there
	 are no more.  */
      mn = opt->back_chain;
    }
  while (mn != cl_options_count);

  /* Return the best wrong match, or cl_options_count if none.  */
  return match_wrong_lang;
}

/* If ARG is a non-negative integer made up solely of digits, return its
   value, otherwise return -1.  */
static int
integral_argument (const char *arg)
{
  const char *p = arg;

  while (*p && ISDIGIT (*p))
    p++;

  if (*p == '\0')
    return atoi (arg);

  return -1;
}

/* Return a malloced slash-separated list of languages in MASK.  */
static char *
write_langs (unsigned int mask)
{
  unsigned int n = 0, len = 0;
  const char *lang_name;
  char *result;

  for (n = 0; (lang_name = lang_names[n]) != 0; n++)
    if (mask & (1U << n))
      len += strlen (lang_name) + 1;

  result = xmalloc (len);
  len = 0;
  for (n = 0; (lang_name = lang_names[n]) != 0; n++)
    if (mask & (1U << n))
      {
	if (len)
	  result[len++] = '/';
	strcpy (result + len, lang_name);
	len += strlen (lang_name);
      }

  result[len] = 0;

  return result;
}

/* Complain that switch OPT_INDEX does not apply to this front end.  */
static void
complain_wrong_lang (const char *text, const struct cl_option *option,
		     unsigned int lang_mask)
{
  char *ok_langs, *bad_lang;

  ok_langs = write_langs (option->flags);
  bad_lang = write_langs (lang_mask);

  /* Eventually this should become a hard error IMO.  */
  warning ("command line option \"%s\" is valid for %s but not for %s",
	   text, ok_langs, bad_lang);

  free (ok_langs);
  free (bad_lang);
}

/* Handle the switch beginning at ARGV for the language indicated by
   LANG_MASK.  Returns the number of switches consumed.  */
static unsigned int
handle_option (const char **argv, unsigned int lang_mask)
{
  size_t opt_index;
  const char *opt, *arg = 0;
  char *dup = 0;
  int value = 1;
  unsigned int result = 0;
  const struct cl_option *option;

  opt = argv[0];

  /* Drop the "no-" from negative switches.  */
  if ((opt[1] == 'W' || opt[1] == 'f')
      && opt[2] == 'n' && opt[3] == 'o' && opt[4] == '-')
    {
      size_t len = strlen (opt) - 3;

      dup = xmalloc (len + 1);
      dup[0] = '-';
      dup[1] = opt[1];
      memcpy (dup + 2, opt + 5, len - 2 + 1);
      opt = dup;
      value = 0;
    }

  opt_index = find_opt (opt + 1, lang_mask | CL_COMMON);
  if (opt_index == cl_options_count)
    goto done;

  option = &cl_options[opt_index];

  /* Reject negative form of switches that don't take negatives as
     unrecognized.  */
  if (!value && (option->flags & CL_REJECT_NEGATIVE))
    goto done;

  /* We've recognized this switch.  */
  result = 1;

  /* Sort out any argument the switch takes.  */
  if (option->flags & CL_JOINED)
    {
      /* Have arg point to the original switch.  This is because
	 some code, such as disable_builtin_function, expects its
	 argument to be persistent until the program exits.  */
      arg = argv[0] + cl_options[opt_index].opt_len + 1;
      if (!value)
	arg += strlen ("no-");

      if (*arg == '\0' && !(option->flags & CL_MISSING_OK))
	{
	  if (option->flags & CL_SEPARATE)
	    {
	      arg = argv[1];
	      result = 2;
	    }
	  else
	    /* Missing argument.  */
	    arg = NULL;
	}
    }
  else if (option->flags & CL_SEPARATE)
    {
      arg = argv[1];
      result = 2;
    }

  /* Now we've swallowed any potential argument, complain if this
     is a switch for a different front end.  */
  if (!(option->flags & (lang_mask | CL_COMMON)))
    {
      complain_wrong_lang (argv[0], option, lang_mask);
      goto done;
    }

  if (arg == NULL && (option->flags & (CL_JOINED | CL_SEPARATE)))
    {
      if (!lang_hooks.missing_argument (opt, opt_index))
	error ("missing argument to \"%s\"", opt);
      goto done;
    }

  /* If the switch takes an integer, convert it.  */
  if (arg && (option->flags & CL_UINTEGER))
    {
      value = integral_argument (arg);
      if (value == -1)
	{
	  error ("argument to \"%s\" should be a non-negative integer",
		 option->opt_text);
	  goto done;
	}
    }

  if (option->flag_var)
    {
      if (option->has_set_value)
	{
	  if (value)
	    *option->flag_var = option->set_value;
	  else
	    *option->flag_var = !option->set_value;
	}
      else
	*option->flag_var = value;
    }
  
  if (option->flags & lang_mask)
    if (lang_hooks.handle_option (opt_index, arg, value) == 0)
      result = 0;

  if (result && (option->flags & CL_COMMON))
    if (common_handle_option (opt_index, arg, value) == 0)
      result = 0;

 done:
  if (dup)
    free (dup);
  return result;
}

/* Decode and handle the vector of command line options.  LANG_MASK
   contains has a single bit set representing the current
   language.  */
static void
handle_options (unsigned int argc, const char **argv, unsigned int lang_mask)
{
  unsigned int n, i;

  for (i = 1; i < argc; i += n)
    {
      const char *opt = argv[i];

      /* Interpret "-" or a non-switch as a file name.  */
      if (opt[0] != '-' || opt[1] == '\0')
	{
	  if (main_input_filename == NULL)
	    main_input_filename = opt;
	  add_input_filename (opt);
	  n = 1;
	  continue;
	}

      n = handle_option (argv + i, lang_mask);

      if (!n)
	{
	  n = 1;
	  error ("unrecognized command line option \"%s\"", opt);
	}
    }
}

/* Handle FILENAME from the command line.  */
void
add_input_filename (const char *filename)
{
  num_in_fnames++;
  in_fnames = xrealloc (in_fnames, num_in_fnames * sizeof (in_fnames[0]));
  in_fnames[num_in_fnames - 1] = filename;
}

/* Parse command line options and set default flag values.  Do minimal
   options processing.  */
void
decode_options (unsigned int argc, const char **argv)
{
  unsigned int i, lang_mask;

  /* Perform language-specific options initialization.  */
  lang_mask = lang_hooks.init_options (argc, argv);

  lang_hooks.initialize_diagnostics (global_dc);

  /* Scan to see what optimization level has been specified.  That will
     determine the default value of many flags.  */
  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-O"))
	{
	  optimize = 1;
	  optimize_size = 0;
	}
      else if (argv[i][0] == '-' && argv[i][1] == 'O')
	{
	  /* Handle -Os, -O2, -O3, -O69, ...  */
	  const char *p = &argv[i][2];

	  if ((p[0] == 's') && (p[1] == 0))
	    {
	      optimize_size = 1;

	      /* Optimizing for size forces optimize to be 2.  */
	      optimize = 2;
	    }
	  else
	    {
	      const int optimize_val = read_integral_parameter (p, p - 2, -1);
	      if (optimize_val != -1)
		{
		  optimize = optimize_val;
		  optimize_size = 0;
		}
	    }
	}
        /* APPLE LOCAL begin -fast or -fastf or -fastcp */
      else if (argv[i][0] == '-' && argv[i][1] == 'f')
        {
          const char *p = &argv[i][2];
          if (!strcmp(p, "ast"))
            flag_fast = 1;
          else if (!strcmp(p, "astf"))
            flag_fastf = 1;
          else if (!strcmp(p, "astcp"))
            flag_fastcp = 1;
        }
        /* APPLE LOCAL end -fast or -fastf or -fastcp */
    }

    /* APPLE LOCAL begin -fast or -fastf or -fastcp */
    if (flag_fast || flag_fastf || flag_fastcp )
    {
      optimize = 3;
      optimize_size = 0;
      /* This goes here, rather than in rs6000.c, so that
	 later -fcommon can override it.  */
      if (flag_fast || flag_fastcp)
        flag_no_common = 1;
    }
    /* APPLE LOCAL end -fast or -fastf or -fastcp */

  if (!optimize)
    {
      flag_merge_constants = 0;
    }

  if (optimize >= 1)
    {
      flag_defer_pop = 1;
      flag_thread_jumps = 1;
#ifdef DELAY_SLOTS
      flag_delayed_branch = 1;
#endif
#ifdef CAN_DEBUG_WITHOUT_FP
      flag_omit_frame_pointer = 1;
#endif
      flag_guess_branch_prob = 1;
      flag_cprop_registers = 1;
      flag_loop_optimize = 1;
      flag_if_conversion = 1;
      flag_if_conversion2 = 1;
      flag_tree_ccp = 1;
      flag_tree_dce = 1;
      flag_tree_dom = 1;
      flag_tree_dse = 1;
      /* APPLE LOCAL begin lno */
      flag_tree_loop_im = 1;
      flag_ivcanon = 1;
      flag_ivopts = 1;
      flag_tree_vectorize = 0;
      flag_tree_loop_linear = 0;
      /* APPLE LOCAL end lno */
      flag_tree_pre = 1;
      /* APPLE LOCAL lno */
      flag_tree_elim_checks = 0;
      flag_tree_ter = 1;
      flag_tree_live_range_split = 1;
      flag_tree_sra = 1;
      flag_tree_copyrename = 1;
      flag_tree_fre = 1;

      if (!optimize_size)
	{
	  /* Loop header copying usually increases size of the code.  This used
	     not to be true, since quite often it is possible to verify that
	     the condition is satisfied in the first iteration and therefore
	     to eliminate it.  Jump threading handles these cases now.  */
	  flag_tree_ch = 1;
	}
    }

  if (optimize >= 2)
    {
      flag_crossjumping = 1;
      flag_optimize_sibling_calls = 1;
      flag_cse_follow_jumps = 1;
      flag_cse_skip_blocks = 1;
      flag_gcse = 1;
      flag_expensive_optimizations = 1;
      flag_strength_reduce = 1;
      flag_rerun_cse_after_loop = 1;
      flag_rerun_loop_opt = 1;
      flag_caller_saves = 1;
      flag_force_mem = 1;
      flag_peephole2 = 1;
#ifdef INSN_SCHEDULING
      flag_schedule_insns = 1;
      flag_schedule_insns_after_reload = 1;
#endif
      flag_regmove = 1;
      flag_strict_aliasing = 1;
      flag_delete_null_pointer_checks = 1;
      flag_reorder_blocks = 1;
      flag_reorder_functions = 1;
      flag_unit_at_a_time = 1;
    }

  if (optimize >= 3)
    {
      flag_inline_functions = 1;
      flag_unswitch_loops = 1;
      flag_gcse_after_reload = 1;
    }

  if (optimize < 2 || optimize_size)
    {
      align_loops = 1;
      align_jumps = 1;
      align_labels = 1;
      align_functions = 1;

      /* Don't reorder blocks when optimizing for size because extra
	 jump insns may be created; also barrier may create extra padding.

	 More correctly we should have a block reordering mode that tried
	 to minimize the combined size of all the jumps.  This would more
	 or less automatically remove extra jumps, but would also try to
	 use more short jumps instead of long jumps.  */
      flag_reorder_blocks = 0;
      flag_reorder_blocks_and_partition = 0;
    }

  if (optimize_size)
    {
      /* Inlining of very small functions usually reduces total size.  */
      set_param_value ("max-inline-insns-single", 5);
      set_param_value ("max-inline-insns-auto", 5);
      set_param_value ("max-inline-insns-rtl", 10);
      flag_inline_functions = 1;
    }

  /* Initialize whether `char' is signed.  */
  flag_signed_char = DEFAULT_SIGNED_CHAR;
  /* Set this to a special "uninitialized" value.  The actual default is set
     after target options have been processed.  */
  flag_short_enums = 2;

  /* Initialize target_flags before OPTIMIZATION_OPTIONS so the latter can
     modify it.  */
  target_flags = 0;
  set_target_switch ("");

  /* Unwind tables are always present when a target has ABI-specified unwind
     tables, so the default should be ON.  */
#ifdef TARGET_UNWIND_INFO
  flag_unwind_tables = TARGET_UNWIND_INFO;
#endif

#ifdef OPTIMIZATION_OPTIONS
  /* Allow default optimizations to be specified on a per-machine basis.  */
  OPTIMIZATION_OPTIONS (optimize, optimize_size);
#endif

  handle_options (argc, argv, lang_mask);

  if (flag_pie)
    flag_pic = flag_pie;
  if (flag_pic && !flag_pie)
    flag_shlib = 1;

  if (flag_no_inline == 2)
    flag_no_inline = 0;
  else
    flag_really_no_inline = flag_no_inline;

  /* Set flag_no_inline before the post_options () hook.  The C front
     ends use it to determine tree inlining defaults.  FIXME: such
     code should be lang-independent when all front ends use tree
     inlining, in which case it, and this condition, should be moved
     to the top of process_options() instead.  */
  if (optimize == 0)
    {
      /* Inlining does not work if not optimizing,
	 so force it not to be done.  */
      flag_no_inline = 1;
      warn_inline = 0;

      /* The c_decode_option function and decode_option hook set
	 this to `2' if -Wall is used, so we can avoid giving out
	 lots of errors for people who don't realize what -Wall does.  */
      if (warn_uninitialized == 1)
	warning ("-Wuninitialized is not supported without -O");
    }

  if (flag_really_no_inline == 2)
    flag_really_no_inline = flag_no_inline;

  /* The optimization to partition hot and cold basic blocks into separate
     sections of the .o and executable files does not work (currently)
     with exception handling.  If flag_exceptions is turned on we need to
     turn off the partitioning optimization.  */

  if (flag_exceptions && flag_reorder_blocks_and_partition)
    {
      warning 
	    ("-freorder-blocks-and-partition does not work with exceptions");
      flag_reorder_blocks_and_partition = 0;
      flag_reorder_blocks = 1;
    }

  /* The optimization to partition hot and cold basic blocks into
     separate sections of the .o and executable files does not currently
     work correctly with DWARF debugging turned on.  Until this is fixed
     we will disable the optimization when DWARF debugging is set.  */
  
  if (flag_reorder_blocks_and_partition && write_symbols == DWARF2_DEBUG)
    {
      warning
	("-freorder-blocks-and-partition does not work with -g (currently)");
      flag_reorder_blocks_and_partition = 0;
      flag_reorder_blocks = 1;
    }
}

/* Handle target- and language-independent options.  Return zero to
   generate an "unknown option" message.  Only options that need
   extra handling need to be listed here; if you simply want
   VALUE assigned to a variable, it happens automatically.  */

static int
common_handle_option (size_t scode, const char *arg, int value)
{
  enum opt_code code = (enum opt_code) scode;

  switch (code)
    {
    /* APPLE LOCAL begin fat builds */
    case OPT_arch:
      /* Ignore for now. */
      break;
    /* APPLE LOCAL end fat builds */

    case OPT__help:
      print_help ();
      exit_after_options = true;
      break;

    case OPT__param:
      handle_param (arg);
      break;

    case OPT__target_help:
      display_target_options ();
      exit_after_options = true;
      break;

    case OPT__version:
      print_version (stderr, "");
      exit_after_options = true;
      break;

    case OPT_G:
      g_switch_value = value;
      g_switch_set = true;
      break;

    case OPT_O:
    case OPT_Os:
      /* Currently handled in a prescan.  */
      break;

    case OPT_W:
      /* For backward compatibility, -W is the same as -Wextra.  */
      set_Wextra (value);
      break;

    case OPT_Wextra:
      set_Wextra (value);
      break;

    case OPT_Wlarger_than_:
      larger_than_size = value;
      warn_larger_than = value != -1;
      break;

    case OPT_Wstrict_aliasing:
    case OPT_Wstrict_aliasing_:
      warn_strict_aliasing = value;
      break;

    case OPT_Wunused:
      set_Wunused (value);
      break;

    case OPT_aux_info:
    case OPT_aux_info_:
      aux_info_file_name = arg;
      flag_gen_aux_info = 1;
      break;

    case OPT_auxbase:
      aux_base_name = arg;
      break;

    case OPT_auxbase_strip:
      {
	char *tmp = xstrdup (arg);
	strip_off_ending (tmp, strlen (tmp));
	if (tmp[0])
	  aux_base_name = tmp;
      }
      break;

    case OPT_d:
      decode_d_option (arg);
      break;

    case OPT_dumpbase:
      dump_base_name = arg;
      break;

    /* APPLE LOCAL begin -floop-transpose */
    case OPT_floop_transpose:
      flag_loop_transpose = value;
      break;
    /* APPLE LOCAL end -floop-transpose */

    case OPT_falign_functions_:
      align_functions = value;
      break;

    case OPT_falign_jumps_:
      align_jumps = value;
      break;

    case OPT_falign_labels_:
      align_labels = value;
      break;

    case OPT_falign_loops_:
      align_loops = value;
      break;

    /* APPLE LOCAL begin predictive compilation */
    case OPT_fpredictive_compilation:
      predictive_compilation = 0;
      break;

    case OPT_fpredictive_compilation_:
      {
	char* buf = xmalloc (strlen(arg) + 1);
	sprintf (buf, "%d", value);
	if (strcmp(buf, arg))
	  {
	    error ("argument to \"-fpredictive-compilation=\" should be a valid non-negative integer instead of \"%s\"", arg);
	    value = 0;
	  }
	free(buf);
        predictive_compilation = value;
        break;
      }
    /* APPLE LOCAL end predictive compilation */

    case OPT_fbranch_probabilities:
      flag_branch_probabilities_set = true;
      break;

    case OPT_fcall_used_:
      fix_register (arg, 0, 1);
      break;

    case OPT_fcall_saved_:
      fix_register (arg, 0, 0);
      break;

    case OPT_fdiagnostics_show_location_:
      if (!strcmp (arg, "once"))
	diagnostic_prefixing_rule (global_dc) = DIAGNOSTICS_SHOW_PREFIX_ONCE;
      else if (!strcmp (arg, "every-line"))
	diagnostic_prefixing_rule (global_dc)
	  = DIAGNOSTICS_SHOW_PREFIX_EVERY_LINE;
      else
	return 0;
      break;

    case OPT_fdump_:
      if (!dump_switch_p (arg))
	return 0;
      break;

    case OPT_ffast_math:
      set_fast_math_flags (value);
      break;

    case OPT_ffixed_:
      fix_register (arg, 1, 1);
      break;

    case OPT_finline_limit_:
    case OPT_finline_limit_eq:
      set_param_value ("max-inline-insns-single", value / 2);
      set_param_value ("max-inline-insns-auto", value / 2);
      set_param_value ("max-inline-insns-rtl", value);
      break;

    case OPT_fmessage_length_:
      pp_set_line_maximum_length (global_dc->printer, value);
      break;

    case OPT_fpack_struct_:
      if (value <= 0 || (value & (value - 1)) || value > 16)
	error("structure alignment must be a small power of two, not %d", value);
      else
	{
	  initial_max_fld_align = value;
	  maximum_field_alignment = value * BITS_PER_UNIT;
	}
      break;

    case OPT_fpeel_loops:
      flag_peel_loops_set = true;
      break;

    case OPT_fprofile_arcs:
      profile_arc_flag_set = true;
      break;

    /* APPLE LOCAL begin add fuse-profile */
    case OPT_fuse_profile:
    /* APPLE LOCAL end add fuse-profile */
    case OPT_fprofile_use:
      if (!flag_branch_probabilities_set)
        flag_branch_probabilities = value;
      if (!flag_profile_values_set)
        flag_profile_values = value;
      if (!flag_unroll_loops_set)
        flag_unroll_loops = value;
      if (!flag_peel_loops_set)
        flag_peel_loops = value;
      if (!flag_tracer_set)
        flag_tracer = value;
      if (!flag_value_profile_transformations_set)
        flag_value_profile_transformations = value;
#ifdef HAVE_prefetch
      if (!flag_speculative_prefetching_set)
	flag_speculative_prefetching = value;
#endif
      break;

    /* APPLE LOCAL begin add fcreate-profile */
    case OPT_fcreate_profile:
    /* APPLE LOCAL end add fcreate-profile */
    case OPT_fprofile_generate:
      if (!profile_arc_flag_set)
        profile_arc_flag = value;
      if (!flag_profile_values_set)
        flag_profile_values = value;
      if (!flag_value_profile_transformations_set)
        flag_value_profile_transformations = value;
#ifdef HAVE_prefetch
      if (!flag_speculative_prefetching_set)
	flag_speculative_prefetching = value;
#endif
      break;

    case OPT_fprofile_values:
      flag_profile_values_set = true;
      break;

    case OPT_fvisibility_:
      {
        if (!strcmp(arg, "default"))
          default_visibility = VISIBILITY_DEFAULT;
        else if (!strcmp(arg, "internal"))
          default_visibility = VISIBILITY_INTERNAL;
        else if (!strcmp(arg, "hidden"))
          default_visibility = VISIBILITY_HIDDEN;
        else if (!strcmp(arg, "protected"))
          default_visibility = VISIBILITY_PROTECTED;
        else
          error ("unrecognised visibility value \"%s\"", arg);
      }
      break;

    case OPT_fvpt:
      flag_value_profile_transformations_set = true;
      break;

    case OPT_fspeculative_prefetching:
      flag_speculative_prefetching_set = true;
      break;

    case OPT_frandom_seed:
      /* The real switch is -fno-random-seed.  */
      if (value)
	return 0;
      flag_random_seed = NULL;
      break;

    case OPT_frandom_seed_:
      flag_random_seed = arg;
      break;

    case OPT_fsched_verbose_:
#ifdef INSN_SCHEDULING
      fix_sched_param ("verbose", arg);
      break;
#else
      return 0;
#endif

    case OPT_fsched_stalled_insns_:
      flag_sched_stalled_insns = value;
      if (flag_sched_stalled_insns == 0)
	flag_sched_stalled_insns = -1;
      break;

    case OPT_fsched_stalled_insns_dep_:
      flag_sched_stalled_insns_dep = value;
      break;

    case OPT_fstack_limit:
      /* The real switch is -fno-stack-limit.  */
      if (value)
	return 0;
      stack_limit_rtx = NULL_RTX;
      break;

    case OPT_fstack_limit_register_:
      {
	int reg = decode_reg_name (arg);
	if (reg < 0)
	  error ("unrecognized register name \"%s\"", arg);
	else
	  stack_limit_rtx = gen_rtx_REG (Pmode, reg);
      }
      break;

    case OPT_fstack_limit_symbol_:
      stack_limit_rtx = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (arg));
      break;

    case OPT_ftls_model_:
      if (!strcmp (arg, "global-dynamic"))
	flag_tls_default = TLS_MODEL_GLOBAL_DYNAMIC;
      else if (!strcmp (arg, "local-dynamic"))
	flag_tls_default = TLS_MODEL_LOCAL_DYNAMIC;
      else if (!strcmp (arg, "initial-exec"))
	flag_tls_default = TLS_MODEL_INITIAL_EXEC;
      else if (!strcmp (arg, "local-exec"))
	flag_tls_default = TLS_MODEL_LOCAL_EXEC;
      else
	warning ("unknown tls-model \"%s\"", arg);
      break;

    case OPT_ftracer:
      flag_tracer_set = true;
      break;

    case OPT_funroll_loops:
      flag_unroll_loops_set = true;
      break;

      /* APPLE LOCAL begin fwritable strings  */
    case OPT_fwritable_strings:
      flag_writable_strings = value;
      break;
      /* APPLE LOCAL end fwritable strings  */

    case OPT_g:
      set_debug_level (NO_DEBUG, DEFAULT_GDB_EXTENSIONS, arg);
      break;

    case OPT_gcoff:
      set_debug_level (SDB_DEBUG, false, arg);
      break;

    case OPT_gdwarf_2:
      set_debug_level (DWARF2_DEBUG, false, arg);
      break;

    case OPT_ggdb:
      set_debug_level (NO_DEBUG, 2, arg);
      break;

    case OPT_gstabs:
    case OPT_gstabs_:
      set_debug_level (DBX_DEBUG, code == OPT_gstabs_, arg);
      break;

    case OPT_gvms:
      set_debug_level (VMS_DEBUG, false, arg);
      break;

    case OPT_gxcoff:
    case OPT_gxcoff_:
      set_debug_level (XCOFF_DEBUG, code == OPT_gxcoff_, arg);
      break;

    case OPT_m:
      set_target_switch (arg);
      break;

    case OPT_o:
      asm_file_name = arg;
      break;

    case OPT_pedantic_errors:
      flag_pedantic_errors = pedantic = 1;
      break;

    default:
      /* If the flag was handled in a standard way, assume the lack of
	 processing here is intentional.  */
      if (cl_options[scode].flag_var)
	break;

      abort ();
    }

  return 1;
}

/* Handle --param NAME=VALUE.  */
static void
handle_param (const char *carg)
{
  char *equal, *arg;
  int value;

  arg = xstrdup (carg);
  equal = strchr (arg, '=');
  if (!equal)
    error ("%s: --param arguments should be of the form NAME=VALUE", arg);
  else
    {
      value = integral_argument (equal + 1);
      if (value == -1)
	error ("invalid --param value `%s'", equal + 1);
      else
	{
	  *equal = '\0';
	  set_param_value (arg, value);
	}
    }

  free (arg);
}

/* Handle -W and -Wextra.  */
static void
set_Wextra (int setting)
{
  extra_warnings = setting;
  warn_unused_value = setting;
  warn_unused_parameter = (setting && maybe_warn_unused_parameter);

  /* We save the value of warn_uninitialized, since if they put
     -Wuninitialized on the command line, we need to generate a
     warning about not using it without also specifying -O.  */
  if (setting == 0)
    warn_uninitialized = 0;
  else if (warn_uninitialized != 1)
    warn_uninitialized = 2;
}

/* Initialize unused warning flags.  */
void
set_Wunused (int setting)
{
  warn_unused_function = setting;
  warn_unused_label = setting;
  /* Unused function parameter warnings are reported when either
     ``-Wextra -Wunused'' or ``-Wunused-parameter'' is specified.
     Thus, if -Wextra has already been seen, set warn_unused_parameter;
     otherwise set maybe_warn_extra_parameter, which will be picked up
     by set_Wextra.  */
  maybe_warn_unused_parameter = setting;
  warn_unused_parameter = (setting && extra_warnings);
  warn_unused_variable = setting;
  warn_unused_value = setting;
}

/* The following routines are useful in setting all the flags that
   -ffast-math and -fno-fast-math imply.  */
void
set_fast_math_flags (int set)
{
  flag_trapping_math = !set;
  flag_unsafe_math_optimizations = set;
  flag_finite_math_only = set;
  flag_errno_math = !set;
  if (set)
    {
      flag_signaling_nans = 0;
      flag_rounding_math = 0;
    }
}

/* Return true iff flags are set as if -ffast-math.  */
bool
fast_math_flags_set_p (void)
{
  return (!flag_trapping_math
	  && flag_unsafe_math_optimizations
	  && flag_finite_math_only
	  && !flag_errno_math);
}

/* Handle a debug output -g switch.  EXTENDED is true or false to support
   extended output (2 is special and means "-ggdb" was given).  */
static void
set_debug_level (enum debug_info_type type, int extended, const char *arg)
{
  static bool type_explicit;

  use_gnu_debug_info_extensions = extended;

  if (type == NO_DEBUG)
    {
      if (write_symbols == NO_DEBUG)
	{
	  write_symbols = PREFERRED_DEBUGGING_TYPE;

	  if (extended == 2)
	    {
#ifdef DWARF2_DEBUGGING_INFO
	      write_symbols = DWARF2_DEBUG;
#elif defined DBX_DEBUGGING_INFO
	      write_symbols = DBX_DEBUG;
#endif
/* APPLE LOCAL begin dwarf */
/* Even though DWARF2_DEBUGGING_INFO is defined, use stabs for
   debugging symbols with -ggdb.  Remove this local patch when we
   switch to dwarf.  */
	      write_symbols = DBX_DEBUG;
/* APPLE LOCAL end dwarf */
	    }

	  if (write_symbols == NO_DEBUG)
	    warning ("target system does not support debug output");
	}
    }
  else
    {
      /* Does it conflict with an already selected type?  */
      if (type_explicit && write_symbols != NO_DEBUG && type != write_symbols)
	error ("debug format \"%s\" conflicts with prior selection",
	       debug_type_names[type]);
      write_symbols = type;
      type_explicit = true;
    }

  /* A debug flag without a level defaults to level 2.  */
  if (*arg == '\0')
    {
      if (!debug_info_level)
	debug_info_level = 2;
    }
  else
    {
      debug_info_level = integral_argument (arg);
      if (debug_info_level == (unsigned int) -1)
	error ("unrecognised debug output level \"%s\"", arg);
      else if (debug_info_level > 3)
	error ("debug output level %s is too high", arg);
    }

}

/* Output --help text.  */
static void
print_help (void)
{
  size_t i;
  const char *p;

  GET_ENVIRONMENT (p, "COLUMNS");
  if (p)
    {
      int value = atoi (p);
      if (value > 0)
	columns = value;
    }

  puts (_("The following options are language-independent:\n"));

  print_filtered_help (CL_COMMON);
  print_param_help ();

  for (i = 0; lang_names[i]; i++)
    {
      printf (_("The %s front end recognizes the following options:\n\n"),
	      lang_names[i]);
      print_filtered_help (1U << i);
    }

  display_target_options ();
}

/* Print the help for --param.  */
static void
print_param_help (void)
{
  size_t i;

  puts (_("The --param option recognizes the following as parameters:\n"));

  for (i = 0; i < LAST_PARAM; i++)
    {
      const char *help = compiler_params[i].help;
      const char *param = compiler_params[i].option;

      if (help == NULL || *help == '\0')
	help = undocumented_msg;

      /* Get the translation.  */
      help = _(help);

      wrap_help (help, param, strlen (param));
    }

  putchar ('\n');
}

/* Print help for a specific front-end, etc.  */
static void
print_filtered_help (unsigned int flag)
{
  unsigned int i, len, filter, indent = 0;
  bool duplicates = false;
  const char *help, *opt, *tab;
  static char *printed;

  if (flag == CL_COMMON)
    {
      filter = flag;
      if (!printed)
	printed = xmalloc (cl_options_count);
      memset (printed, 0, cl_options_count);
    }
  else
    {
      /* Don't print COMMON options twice.  */
      filter = flag | CL_COMMON;

      for (i = 0; i < cl_options_count; i++)
	{
	  if ((cl_options[i].flags & filter) != flag)
	    continue;

	  /* Skip help for internal switches.  */
	  if (cl_options[i].flags & CL_UNDOCUMENTED)
	    continue;

	  /* Skip switches that have already been printed, mark them to be
	     listed later.  */
	  if (printed[i])
	    {
	      duplicates = true;
	      indent = print_switch (cl_options[i].opt_text, indent);
	    }
	}

      if (duplicates)
	{
	  putchar ('\n');
	  putchar ('\n');
	}
    }

  for (i = 0; i < cl_options_count; i++)
    {
      if ((cl_options[i].flags & filter) != flag)
	continue;

      /* Skip help for internal switches.  */
      if (cl_options[i].flags & CL_UNDOCUMENTED)
	continue;

      /* Skip switches that have already been printed.  */
      if (printed[i])
	continue;

      printed[i] = true;

      help = cl_options[i].help;
      if (!help)
	help = undocumented_msg;

      /* Get the translation.  */
      help = _(help);

      tab = strchr (help, '\t');
      if (tab)
	{
	  len = tab - help;
	  opt = help;
	  help = tab + 1;
	}
      else
	{
	  opt = cl_options[i].opt_text;
	  len = strlen (opt);
	}

      wrap_help (help, opt, len);
    }

  putchar ('\n');
}

/* Output ITEM, of length ITEM_WIDTH, in the left column, followed by
   word-wrapped HELP in a second column.  */
static unsigned int
print_switch (const char *text, unsigned int indent)
{
  unsigned int len = strlen (text) + 1; /* trailing comma */

  if (indent)
    {
      putchar (',');
      if (indent + len > columns)
	{
	  putchar ('\n');
	  putchar (' ');
	  indent = 1;
	}
    }
  else
    putchar (' ');

  putchar (' ');
  fputs (text, stdout);

  return indent + len + 1;
}

/* Output ITEM, of length ITEM_WIDTH, in the left column, followed by
   word-wrapped HELP in a second column.  */
static void
wrap_help (const char *help, const char *item, unsigned int item_width)
{
  unsigned int col_width = 27;
  unsigned int remaining, room, len;

  remaining = strlen (help);

  do
    {
      room = columns - 3 - MAX (col_width, item_width);
      if (room > columns)
	room = 0;
      len = remaining;

      if (room < len)
	{
	  unsigned int i;

	  for (i = 0; help[i]; i++)
	    {
	      if (i >= room && len != remaining)
		break;
	      if (help[i] == ' ')
		len = i;
	      else if ((help[i] == '-' || help[i] == '/')
		       && help[i + 1] != ' '
		       && i > 0 && ISALPHA (help[i - 1]))
		len = i + 1;
	    }
	}

      printf( "  %-*.*s %.*s\n", col_width, item_width, item, len, help);
      item_width = 0;
      while (help[len] == ' ')
	len++;
      help += len;
      remaining -= len;
    }
  while (remaining);
}
