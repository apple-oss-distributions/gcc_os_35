------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--                       A D A . E X C E P T I O N S                        --
--                                                                          --
--                                 B o d y                                  --
--                                                                          --
--          Copyright (C) 1992-2004 Free Software Foundation, Inc.          --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 2,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License --
-- for  more details.  You should have  received  a copy of the GNU General --
-- Public License  distributed with GNAT;  see file COPYING.  If not, write --
-- to  the Free Software Foundation,  59 Temple Place - Suite 330,  Boston, --
-- MA 02111-1307, USA.                                                      --
--                                                                          --
-- As a special exception,  if other files  instantiate  generics from this --
-- unit, or you link  this unit with other files  to produce an executable, --
-- this  unit  does not  by itself cause  the resulting  executable  to  be --
-- covered  by the  GNU  General  Public  License.  This exception does not --
-- however invalidate  any other reasons why  the executable file  might be --
-- covered by the  GNU Public License.                                      --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

pragma Polling (Off);
--  We must turn polling off for this unit, because otherwise we get
--  elaboration circularities with System.Exception_Tables.

pragma Warnings (Off);
--  Since several constructs give warnings in 3.14a1, including unreferenced
--  variables and pragma Unreferenced itself.

with System;                  use System;
with System.Standard_Library; use System.Standard_Library;
with System.Soft_Links;       use System.Soft_Links;
with System.Machine_State_Operations; use System.Machine_State_Operations;

package body Ada.Exceptions is

   procedure builtin_longjmp (buffer : Address; Flag : Integer);
   pragma No_Return (builtin_longjmp);
   pragma Import (C, builtin_longjmp, "_gnat_builtin_longjmp");

   pragma Suppress (All_Checks);
   --  We definitely do not want exceptions occurring within this unit, or
   --  we are in big trouble. If an exceptional situation does occur, better
   --  that it not be raised, since raising it can cause confusing chaos.

   Zero_Cost_Exceptions : Integer;
   pragma Import (C, Zero_Cost_Exceptions, "__gl_zero_cost_exceptions");
   --  Boolean indicating if we are handling exceptions using a zero cost
   --  mechanism.
   --
   --  Note that although we currently do not support it, the GCC3 back-end
   --  tables are also potentially useable for setjmp/longjmp processing.

   -----------------------
   -- Local Subprograms --
   -----------------------

   --  Note: the exported subprograms in this package body are called directly
   --  from C clients using the given external name, even though they are not
   --  technically visible in the Ada sense.

   procedure AAA;
   procedure ZZZ;
   --  Mark start and end of procedures in this package
   --
   --  The AAA and ZZZ procedures are used to provide exclusion bounds in
   --  calls to Call_Chain at exception raise points from this unit. The
   --  purpose is to arrange for the exception tracebacks not to include
   --  frames from routines involved in the raise process, as these are
   --  meaningless from the user's standpoint.
   --
   --  For these bounds to be meaningful, we need to ensure that the object
   --  code for the routines involved in processing a raise is located after
   --  the object code for AAA and before the object code for ZZZ. This will
   --  indeed be the case as long as the following rules are respected:
   --
   --  1) The bodies of the subprograms involved in processing a raise
   --     are located after the body of AAA and before the body of ZZZ.
   --
   --  2) No pragma Inline applies to any of these subprograms, as this
   --     could delay the corresponding assembly output until the end of
   --     the unit.

   Code_Address_For_AAA, Code_Address_For_ZZZ : System.Address;
   --  Used to represent addresses really inside the code range for AAA and
   --  ZZZ, initialized to the address of a label inside the corresponding
   --  procedure. This is initialization takes place inside the procedures
   --  themselves, which are called as part of the elaboration code.
   --
   --  We are doing this instead of merely using Proc'Address because on some
   --  platforms the latter does not yield the address we want, but the
   --  address of a stub or of a descriptor instead. This is the case at least
   --  on Alpha-VMS and PA-HPUX.

   procedure Call_Chain (Excep : EOA);
   --  Store up to Max_Tracebacks in Excep, corresponding to the current
   --  call chain.

   procedure Process_Raise_Exception
     (E                   : Exception_Id;
      From_Signal_Handler : Boolean);
   pragma No_Return (Process_Raise_Exception);
   --  This is the lowest level raise routine. It raises the exception
   --  referenced by Current_Excep.all in the TSD, without deferring abort
   --  (the caller must ensure that abort is deferred on entry).
   --
   --  This is the common implementation for Raise_Current_Excep and
   --  Raise_From_Signal_Handler. The origin of the call is indicated by the
   --  From_Signal_Handler argument.

   procedure To_Stderr (S : String);
   pragma Export (Ada, To_Stderr, "__gnat_to_stderr");
   --  Little routine to output string to stderr that is also used
   --  in the tasking run time.

   procedure To_Stderr (C : Character);
   pragma Inline (To_Stderr);
   pragma Export (Ada, To_Stderr, "__gnat_to_stderr_char");
   --  Little routine to output a character to stderr, used by some of
   --  the separate units below.

   package Exception_Data is

      ---------------------------------
      -- Exception messages routines --
      ---------------------------------

      procedure Set_Exception_C_Msg
        (Id   : Exception_Id;
         Msg1 : Big_String_Ptr;
         Line : Integer        := 0;
         Msg2 : Big_String_Ptr := null);
      --  This routine is called to setup the exception referenced by the
      --  Current_Excep field in the TSD to contain the indicated Id value
      --  and message. Msg1 is a null terminated string which is generated
      --  as the exception message. If line is non-zero, then a colon and
      --  the decimal representation of this integer is appended to the
      --  message. When Msg2 is non-null, a space and this additional null
      --  terminated string is added to the message.

      procedure Set_Exception_Msg
        (Id      : Exception_Id;
         Message : String);
      --  This routine is called to setup the exception referenced by the
      --  Current_Excep field in the TSD to contain the indicated Id value
      --  and message. Message is a string which is generated as the
      --  exception message.

      --------------------------------------
      -- Exception information subprogram --
      --------------------------------------

      function Exception_Information (X : Exception_Occurrence) return String;
      --  The format of the exception information is as follows:
      --
      --    Exception_Name: <exception name> (as in Exception_Name)
      --    Message: <message> (only if Exception_Message is empty)
      --    PID=nnnn (only if != 0)
      --    Call stack traceback locations:  (only if at least one location)
      --    <0xyyyyyyyy 0xyyyyyyyy ...>      (is recorded)
      --
      --  The lines are separated by a ASCII.LF character.
      --  The nnnn is the partition Id given as decimal digits.
      --  The 0x... line represents traceback program counter locations, in
      --  execution order with the first one being the exception location. It
      --  is present only
      --
      --  The Exception_Name and Message lines are omitted in the abort
      --  signal case, since this is not really an exception.

      --  !! If the format of the generated string is changed, please note
      --  !! that an equivalent modification to the routine String_To_EO must
      --  !! be made to preserve proper functioning of the stream attributes.

      ---------------------------------------
      -- Exception backtracing subprograms --
      ---------------------------------------

      --  What is automatically output when exception tracing is on is the
      --  usual exception information with the call chain backtrace possibly
      --  tailored by a backtrace decorator. Modifying Exception_Information
      --  itself is not a good idea because the decorated output is completely
      --  out of control and would break all our code related to the streaming
      --  of exceptions.  We then provide an alternative function to compute
      --  the possibly tailored output, which is equivalent if no decorator is
      --  currently set:

      function Tailored_Exception_Information
        (X : Exception_Occurrence) return String;
      --  Exception information to be output in the case of automatic tracing
      --  requested through GNAT.Exception_Traces.
      --
      --  This is the same as Exception_Information if no backtrace decorator
      --  is currently in place. Otherwise, this is Exception_Information with
      --  the call chain raw addresses replaced by the result of a call to the
      --  current decorator provided with the call chain addresses.

      pragma Export
        (Ada, Tailored_Exception_Information,
           "__gnat_tailored_exception_information");
      --  This is currently used by System.Tasking.Stages.

   end Exception_Data;

   package Exception_Traces is

      use Exception_Data;
      --  Imports Tailored_Exception_Information

      ----------------------------------------------
      -- Run-Time Exception Notification Routines --
      ----------------------------------------------

      --  These subprograms provide a common run-time interface to trigger the
      --  actions required when an exception is about to be propagated (e.g.
      --  user specified actions or output of exception information). They are
      --  exported to be usable by the Ada exception handling personality
      --  routine when the GCC 3 mechanism is used.

      procedure Notify_Handled_Exception;
      pragma Export
        (C, Notify_Handled_Exception, "__gnat_notify_handled_exception");
      --  This routine is called for a handled occurrence is about to be
      --  propagated.

      procedure Notify_Unhandled_Exception;
      pragma Export
        (C, Notify_Unhandled_Exception, "__gnat_notify_unhandled_exception");
      --  This routine is called when an unhandled occurrence is about to be
      --  propagated.

      procedure Unhandled_Exception_Terminate;
      pragma No_Return (Unhandled_Exception_Terminate);
      --  This procedure is called to terminate execution following an
      --  unhandled exception. The exception information, including
      --  traceback if available is output, and execution is then
      --  terminated. Note that at the point where this routine is
      --  called, the stack has typically been destroyed.

   end Exception_Traces;

   package Exception_Propagation is

      use Exception_Traces;
      --  Imports Notify_Unhandled_Exception and
      --  Unhandled_Exception_Terminate

      ------------------------------------
      -- Exception propagation routines --
      ------------------------------------

      procedure Setup_Exception
        (Excep    : EOA;
         Current  : EOA;
         Reraised : Boolean := False);
      --  Perform the necessary operations to prepare the propagation of Excep
      --  in a task where Current is the current occurrence. Excep is assumed
      --  to be a valid (non null) pointer.
      --
      --  This should be called before any (re-)setting of the current
      --  occurrence. Any such (re-)setting shall take care *not* to clobber
      --  the Private_Data component.
      --
      --  Having Current provided as an argument (instead of retrieving it via
      --  Get_Current_Excep internally) is required to allow one task to setup
      --  an exception for another task, which is used by Transfer_Occurrence.

      procedure Propagate_Exception (From_Signal_Handler : Boolean);
      pragma No_Return (Propagate_Exception);
      --  This procedure propagates the exception represented by the occurrence
      --  referenced by Current_Excep in the TSD for the current task.

   end Exception_Propagation;

   package Stream_Attributes is

      --------------------------------
      -- Stream attributes routines --
      --------------------------------

      function EId_To_String (X : Exception_Id) return String;
      function String_To_EId (S : String) return Exception_Id;
      --  Functions for implementing Exception_Id stream attributes

      function EO_To_String (X : Exception_Occurrence) return String;
      function String_To_EO (S : String) return Exception_Occurrence;
      --  Functions for implementing Exception_Occurrence stream
      --  attributes

   end Stream_Attributes;

   procedure Raise_Current_Excep (E : Exception_Id);
   pragma No_Return (Raise_Current_Excep);
   pragma Export (C, Raise_Current_Excep, "__gnat_raise_nodefer_with_msg");
   --  This is a simple wrapper to Process_Raise_Exception setting the
   --  From_Signal_Handler argument to False.
   --
   --  This external name for Raise_Current_Excep is historical, and probably
   --  should be changed but for now we keep it, because gdb and gigi know
   --  about it.

   procedure Raise_Exception_No_Defer
      (E : Exception_Id; Message : String := "");
   pragma Export
    (Ada, Raise_Exception_No_Defer,
     "ada__exceptions__raise_exception_no_defer");
   pragma No_Return (Raise_Exception_No_Defer);
   --  Similar to Raise_Exception, but with no abort deferral

   procedure Raise_With_Msg (E : Exception_Id);
   pragma No_Return (Raise_With_Msg);
   pragma Export (C, Raise_With_Msg, "__gnat_raise_with_msg");
   --  Raises an exception with given exception id value. A message
   --  is associated with the raise, and has already been stored in the
   --  exception occurrence referenced by the Current_Excep in the TSD.
   --  Abort is deferred before the raise call.

   procedure Raise_With_Location_And_Msg
     (E : Exception_Id;
      F : Big_String_Ptr;
      L : Integer;
      M : Big_String_Ptr := null);
   pragma No_Return (Raise_With_Location_And_Msg);
   --  Raise an exception with given exception id value. A filename and line
   --  number is associated with the raise and is stored in the exception
   --  occurrence and in addition a string message M is appended to
   --  this (if M is not null).

   procedure Raise_Constraint_Error
     (File : Big_String_Ptr;
      Line : Integer);
   pragma No_Return (Raise_Constraint_Error);
   pragma Export
     (C, Raise_Constraint_Error, "__gnat_raise_constraint_error");
   --  Raise constraint error with file:line information

   procedure Raise_Constraint_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr);
   pragma No_Return (Raise_Constraint_Error_Msg);
   pragma Export
     (C, Raise_Constraint_Error_Msg, "__gnat_raise_constraint_error_msg");
   --  Raise constraint error with file:line + msg information

   procedure Raise_Program_Error
     (File : Big_String_Ptr;
      Line : Integer);
   pragma No_Return (Raise_Program_Error);
   pragma Export
     (C, Raise_Program_Error, "__gnat_raise_program_error");
   --  Raise program error with file:line information

   procedure Raise_Program_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr);
   pragma No_Return (Raise_Program_Error_Msg);
   pragma Export
     (C, Raise_Program_Error_Msg, "__gnat_raise_program_error_msg");
   --  Raise program error with file:line + msg information

   procedure Raise_Storage_Error
     (File : Big_String_Ptr;
      Line : Integer);
   pragma No_Return (Raise_Storage_Error);
   pragma Export
     (C, Raise_Storage_Error, "__gnat_raise_storage_error");
   --  Raise storage error with file:line information

   procedure Raise_Storage_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr);
   pragma No_Return (Raise_Storage_Error_Msg);
   pragma Export
     (C, Raise_Storage_Error_Msg, "__gnat_raise_storage_error_msg");
   --  Raise storage error with file:line + reason msg information

   --  The exception raising process and the automatic tracing mechanism rely
   --  on some careful use of flags attached to the exception occurrence. The
   --  graph below illustrates the relations between the Raise_ subprograms
   --  and identifies the points where basic flags such as Exception_Raised
   --  are initialized.
   --
   --  (i) signs indicate the flags initialization points. R stands for Raise,
   --  W for With, and E for Exception.
   --
   --                   R_No_Msg    R_E   R_Pe  R_Ce  R_Se
   --                       |        |     |     |     |
   --                       +--+  +--+     +---+ | +---+
   --                          |  |            | | |
   --     R_E_No_Defer(i)    R_W_Msg(i)       R_W_Loc
   --           |               |              |   |
   --           +------------+  |  +-----------+   +--+
   --                        |  |  |                  |
   --                        |  |  |             Set_E_C_Msg(i)
   --                        |  |  |
   --                   Raise_Current_Excep

   procedure Reraise;
   pragma No_Return (Reraise);
   pragma Export (C, Reraise, "__gnat_reraise");
   --  Reraises the exception referenced by the Current_Excep field of
   --  the TSD (all fields of this exception occurrence are set). Abort
   --  is deferred before the reraise operation.

   --  Save_Occurrence variations: As the management of the private data
   --  attached to occurrences is delicate, wether or not pointers to such
   --  data has to be copied in various situations is better made explicit.
   --  The following procedures provide an internal interface to help making
   --  this explicit.

   procedure Save_Occurrence_And_Private
     (Target : out Exception_Occurrence;
      Source : Exception_Occurrence);
   --  Copy all the components of Source to Target as well as the
   --  Private_Data pointer.

   procedure Save_Occurrence_No_Private
     (Target : out Exception_Occurrence;
      Source : Exception_Occurrence);
   --  Copy all the components of Source to Target, except the
   --  Private_Data pointer.

   procedure Transfer_Occurrence
     (Target : Exception_Occurrence_Access;
      Source : Exception_Occurrence);
   pragma Export (C, Transfer_Occurrence, "__gnat_transfer_occurrence");
   --  Called from System.Tasking.RendezVous.Exceptional_Complete_RendezVous
   --  to setup Target from Source as an exception to be propagated in the
   --  caller task. Target is expected to be a pointer to the fixed TSD
   --  occurrence for this task.

   -----------------------------
   -- Run-Time Check Routines --
   -----------------------------

   --  These routines are called from the runtime to raise a specific
   --  exception with a reason message attached. The parameters are
   --  the file name and line number in each case. The names are keyed
   --  to the codes defined in Types.ads and a-types.h (for example,
   --  the name Rcheck_05 refers to the Reason whose Pos code is 5).

   procedure Rcheck_00 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_01 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_02 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_03 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_04 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_05 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_06 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_07 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_08 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_09 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_10 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_11 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_12 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_13 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_14 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_15 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_16 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_17 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_18 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_19 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_20 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_21 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_22 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_23 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_24 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_25 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_26 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_27 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_28 (File : Big_String_Ptr; Line : Integer);
   procedure Rcheck_29 (File : Big_String_Ptr; Line : Integer);

   pragma Export (C, Rcheck_00, "__gnat_rcheck_00");
   pragma Export (C, Rcheck_01, "__gnat_rcheck_01");
   pragma Export (C, Rcheck_02, "__gnat_rcheck_02");
   pragma Export (C, Rcheck_03, "__gnat_rcheck_03");
   pragma Export (C, Rcheck_04, "__gnat_rcheck_04");
   pragma Export (C, Rcheck_05, "__gnat_rcheck_05");
   pragma Export (C, Rcheck_06, "__gnat_rcheck_06");
   pragma Export (C, Rcheck_07, "__gnat_rcheck_07");
   pragma Export (C, Rcheck_08, "__gnat_rcheck_08");
   pragma Export (C, Rcheck_09, "__gnat_rcheck_09");
   pragma Export (C, Rcheck_10, "__gnat_rcheck_10");
   pragma Export (C, Rcheck_11, "__gnat_rcheck_11");
   pragma Export (C, Rcheck_12, "__gnat_rcheck_12");
   pragma Export (C, Rcheck_13, "__gnat_rcheck_13");
   pragma Export (C, Rcheck_14, "__gnat_rcheck_14");
   pragma Export (C, Rcheck_15, "__gnat_rcheck_15");
   pragma Export (C, Rcheck_16, "__gnat_rcheck_16");
   pragma Export (C, Rcheck_17, "__gnat_rcheck_17");
   pragma Export (C, Rcheck_18, "__gnat_rcheck_18");
   pragma Export (C, Rcheck_19, "__gnat_rcheck_19");
   pragma Export (C, Rcheck_20, "__gnat_rcheck_20");
   pragma Export (C, Rcheck_21, "__gnat_rcheck_21");
   pragma Export (C, Rcheck_22, "__gnat_rcheck_22");
   pragma Export (C, Rcheck_23, "__gnat_rcheck_23");
   pragma Export (C, Rcheck_24, "__gnat_rcheck_24");
   pragma Export (C, Rcheck_25, "__gnat_rcheck_25");
   pragma Export (C, Rcheck_26, "__gnat_rcheck_26");
   pragma Export (C, Rcheck_27, "__gnat_rcheck_27");
   pragma Export (C, Rcheck_28, "__gnat_rcheck_28");
   pragma Export (C, Rcheck_29, "__gnat_rcheck_29");

   pragma No_Return (Rcheck_00);
   pragma No_Return (Rcheck_01);
   pragma No_Return (Rcheck_02);
   pragma No_Return (Rcheck_03);
   pragma No_Return (Rcheck_04);
   pragma No_Return (Rcheck_05);
   pragma No_Return (Rcheck_06);
   pragma No_Return (Rcheck_07);
   pragma No_Return (Rcheck_08);
   pragma No_Return (Rcheck_09);
   pragma No_Return (Rcheck_10);
   pragma No_Return (Rcheck_11);
   pragma No_Return (Rcheck_12);
   pragma No_Return (Rcheck_13);
   pragma No_Return (Rcheck_14);
   pragma No_Return (Rcheck_15);
   pragma No_Return (Rcheck_16);
   pragma No_Return (Rcheck_17);
   pragma No_Return (Rcheck_18);
   pragma No_Return (Rcheck_19);
   pragma No_Return (Rcheck_20);
   pragma No_Return (Rcheck_21);
   pragma No_Return (Rcheck_22);
   pragma No_Return (Rcheck_23);
   pragma No_Return (Rcheck_24);
   pragma No_Return (Rcheck_25);
   pragma No_Return (Rcheck_26);
   pragma No_Return (Rcheck_27);
   pragma No_Return (Rcheck_28);
   pragma No_Return (Rcheck_29);

   ---------------------------------------------
   -- Reason Strings for Run-Time Check Calls --
   ---------------------------------------------

   --  These strings are null-terminated and are used by Rcheck_nn. The
   --  strings correspond to the definitions for Types.RT_Exception_Code.

   use ASCII;

   Rmsg_00 : constant String := "access check failed"              & NUL;
   Rmsg_01 : constant String := "access parameter is null"         & NUL;
   Rmsg_02 : constant String := "discriminant check failed"        & NUL;
   Rmsg_03 : constant String := "divide by zero"                   & NUL;
   Rmsg_04 : constant String := "explicit raise"                   & NUL;
   Rmsg_05 : constant String := "index check failed"               & NUL;
   Rmsg_06 : constant String := "invalid data"                     & NUL;
   Rmsg_07 : constant String := "length check failed"              & NUL;
   Rmsg_08 : constant String := "overflow check failed"            & NUL;
   Rmsg_09 : constant String := "partition check failed"           & NUL;
   Rmsg_10 : constant String := "range check failed"               & NUL;
   Rmsg_11 : constant String := "tag check failed"                 & NUL;
   Rmsg_12 : constant String := "access before elaboration"        & NUL;
   Rmsg_13 : constant String := "accessibility check failed"       & NUL;
   Rmsg_14 : constant String := "all guards closed"                & NUL;
   Rmsg_15 : constant String := "duplicated entry address"         & NUL;
   Rmsg_16 : constant String := "explicit raise"                   & NUL;
   Rmsg_17 : constant String := "finalize/adjust raised exception" & NUL;
   Rmsg_18 : constant String := "misaligned address value"         & NUL;
   Rmsg_19 : constant String := "missing return"                   & NUL;
   Rmsg_20 : constant String := "overlaid controlled object"       & NUL;
   Rmsg_21 : constant String := "potentially blocking operation"   & NUL;
   Rmsg_22 : constant String := "stubbed subprogram called"        & NUL;
   Rmsg_23 : constant String := "unchecked union restriction"      & NUL;
   Rmsg_24 : constant String := "illegal use of"
             & " remote access-to-class-wide type, see RM E.4(18)" & NUL;
   Rmsg_25 : constant String := "empty storage pool"               & NUL;
   Rmsg_26 : constant String := "explicit raise"                   & NUL;
   Rmsg_27 : constant String := "infinite recursion"               & NUL;
   Rmsg_28 : constant String := "object too large"                 & NUL;
   Rmsg_29 : constant String := "restriction violation"            & NUL;

   -----------------------
   -- Polling Interface --
   -----------------------

   type Unsigned is mod 2 ** 32;

   Counter : Unsigned := 0;
   pragma Warnings (Off, Counter);
   --  This counter is provided for convenience. It can be used in Poll to
   --  perform periodic but not systematic operations.

   procedure Poll is separate;
   --  The actual polling routine is separate, so that it can easily
   --  be replaced with a target dependent version.

   ---------
   -- AAA --
   ---------

   --  This dummy procedure gives us the start of the PC range for addresses
   --  within the exception unit itself. We hope that gigi/gcc keep all the
   --  procedures in their original order!

   procedure AAA is
   begin
      <<Start_Of_AAA>>
      Code_Address_For_AAA := Start_Of_AAA'Address;
   end AAA;

   ----------------
   -- Call_Chain --
   ----------------

   procedure Call_Chain (Excep : EOA) is separate;
   --  The actual Call_Chain routine is separate, so that it can easily
   --  be dummied out when no exception traceback information is needed.

   ------------------------------
   -- Current_Target_Exception --
   ------------------------------

   function Current_Target_Exception return Exception_Occurrence is
   begin
      return Null_Occurrence;
   end Current_Target_Exception;

   -------------------
   -- EId_To_String --
   -------------------

   function EId_To_String (X : Exception_Id) return String
     renames Stream_Attributes.EId_To_String;

   ------------------
   -- EO_To_String --
   ------------------

   --  We use the null string to represent the null occurrence, otherwise
   --  we output the Exception_Information string for the occurrence.

   function EO_To_String (X : Exception_Occurrence) return String
     renames Stream_Attributes.EO_To_String;

   ------------------------
   -- Exception_Identity --
   ------------------------

   function Exception_Identity
     (X    : Exception_Occurrence)
      return Exception_Id
   is
   begin
      if X.Id = Null_Id then
         raise Constraint_Error;
      else
         return X.Id;
      end if;
   end Exception_Identity;

   ---------------------------
   -- Exception_Information --
   ---------------------------

   function Exception_Information (X : Exception_Occurrence) return String is
   begin
      if X.Id = Null_Id then
         raise Constraint_Error;
      end if;

      return Exception_Data.Exception_Information (X);
   end Exception_Information;

   -----------------------
   -- Exception_Message --
   -----------------------

   function Exception_Message (X : Exception_Occurrence) return String is
   begin
      if X.Id = Null_Id then
         raise Constraint_Error;
      end if;

      return X.Msg (1 .. X.Msg_Length);
   end Exception_Message;

   --------------------
   -- Exception_Name --
   --------------------

   function Exception_Name (Id : Exception_Id) return String is
   begin
      if Id = null then
         raise Constraint_Error;
      end if;

      return Id.Full_Name.all (1 .. Id.Name_Length - 1);
   end Exception_Name;

   function Exception_Name (X : Exception_Occurrence) return String is
   begin
      return Exception_Name (X.Id);
   end Exception_Name;

   ---------------------------
   -- Exception_Name_Simple --
   ---------------------------

   function Exception_Name_Simple (X : Exception_Occurrence) return String is
      Name : constant String := Exception_Name (X);
      P    : Natural;

   begin
      P := Name'Length;
      while P > 1 loop
         exit when Name (P - 1) = '.';
         P := P - 1;
      end loop;

      --  Return result making sure lower bound is 1

      declare
         subtype Rname is String (1 .. Name'Length - P + 1);
      begin
         return Rname (Name (P .. Name'Length));
      end;
   end Exception_Name_Simple;

   --------------------
   -- Exception_Data --
   --------------------

   package body Exception_Data is separate;
   --  This package can be easily dummied out if we do not want the
   --  basic support for exception messages (such as in Ada 83).

   ---------------------------
   -- Exception_Propagation --
   ---------------------------

   package body Exception_Propagation is separate;
   --  Depending on the actual exception mechanism used (front-end or
   --  back-end based), the implementation will differ, which is why this
   --  package is separated.

   ----------------------
   -- Exception_Traces --
   ----------------------

   package body Exception_Traces is separate;
   --  Depending on the underlying support for IO the implementation
   --  will differ. Moreover we would like to dummy out this package
   --  in case we do not want any exception tracing support. This is
   --  why this package is separated.

   -----------------------
   -- Stream Attributes --
   -----------------------

   package body Stream_Attributes is separate;
   --  This package can be easily dummied out if we do not want the
   --  support for streaming Exception_Ids and Exception_Occurrences.

   -----------------------------
   -- Process_Raise_Exception --
   -----------------------------

   procedure Process_Raise_Exception
     (E                   : Exception_Id;
      From_Signal_Handler : Boolean)
   is
      pragma Inspection_Point (E);
      --  This is so the debugger can reliably inspect the parameter

      Jumpbuf_Ptr : constant Address := Get_Jmpbuf_Address.all;
      Excep       : EOA := Get_Current_Excep.all;

   begin
      --  WARNING : There should be no exception handler for this body
      --  because this would cause gigi to prepend a setup for a new
      --  jmpbuf to the sequence of statements. We would then always get
      --  this new buf in Jumpbuf_Ptr instead of the one for the exception
      --  we are handling, which would completely break the whole design
      --  of this procedure.

      --  Processing varies between zero cost and setjmp/lonjmp processing.

      if Zero_Cost_Exceptions /= 0 then

         --  Use the front-end tables to propagate if we have them, otherwise
         --  resort to the GCC back-end alternative. Backtrace computation is
         --  performed, if required, by the underlying routine. Notifications
         --  for the debugger are also not performed here, because we do not
         --  yet know if the exception is handled.

         Exception_Propagation.Propagate_Exception (From_Signal_Handler);

      else
         --  Compute the backtrace for this occurrence if the corresponding
         --  binder option has been set. Call_Chain takes care of the reraise
         --  case.

         Call_Chain (Excep);
         --  We used to only do this if From_Signal_Handler was not set,
         --  based on the assumption that backtracing from a signal handler
         --  would not work due to stack layout oddities. However, since
         --
         --   1. The flag is never set in tasking programs (Notify_Exception
         --      performs regular raise statements), and
         --
         --   2. No problem has shown up in tasking programs around here so
         --      far, this turned out to be too strong an assumption.
         --
         --  As, in addition, the test was
         --
         --   1. preventing the production of backtraces in non-tasking
         --      programs, and
         --
         --   2. introducing a behavior inconsistency between
         --      the tasking and non-tasking cases,
         --
         --  we have simply removed it.

         --  If the jump buffer pointer is non-null, transfer control using
         --  it. Otherwise announce an unhandled exception (note that this
         --  means that we have no finalizations to do other than at the outer
         --  level). Perform the necessary notification tasks in both cases.

         if Jumpbuf_Ptr /= Null_Address then

            if not Excep.Exception_Raised then
               Excep.Exception_Raised := True;
               Exception_Traces.Notify_Handled_Exception;
            end if;

            builtin_longjmp (Jumpbuf_Ptr, 1);

         else
            --  The pragma Inspection point here ensures that the debugger
            --  can inspect the parameter.

            pragma Inspection_Point (E);

            Exception_Traces.Notify_Unhandled_Exception;
            Exception_Traces.Unhandled_Exception_Terminate;
         end if;
      end if;
   end Process_Raise_Exception;

   ----------------------------
   -- Raise_Constraint_Error --
   ----------------------------

   procedure Raise_Constraint_Error
     (File : Big_String_Ptr;
      Line : Integer)
   is
   begin
      Raise_With_Location_And_Msg
        (Constraint_Error_Def'Access, File, Line);
   end Raise_Constraint_Error;

   --------------------------------
   -- Raise_Constraint_Error_Msg --
   --------------------------------

   procedure Raise_Constraint_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr)
   is
   begin
      Raise_With_Location_And_Msg
        (Constraint_Error_Def'Access, File, Line, Msg);
   end Raise_Constraint_Error_Msg;

   -------------------------
   -- Raise_Current_Excep --
   -------------------------

   procedure Raise_Current_Excep (E : Exception_Id) is
      pragma Inspection_Point (E);
      --  This is so the debugger can reliably inspect the parameter
   begin
      Process_Raise_Exception (E => E, From_Signal_Handler => False);
   end Raise_Current_Excep;

   ---------------------
   -- Raise_Exception --
   ---------------------

   procedure Raise_Exception
     (E       : Exception_Id;
      Message : String := "")
   is
   begin
      if E /= null then
         Exception_Data.Set_Exception_Msg (E, Message);
         Abort_Defer.all;
         Raise_Current_Excep (E);
      end if;
   end Raise_Exception;

   ----------------------------
   -- Raise_Exception_Always --
   ----------------------------

   procedure Raise_Exception_Always
     (E       : Exception_Id;
      Message : String := "")
   is
   begin
      Exception_Data.Set_Exception_Msg (E, Message);
      Abort_Defer.all;
      Raise_Current_Excep (E);
   end Raise_Exception_Always;

   -------------------------------
   -- Raise_From_Signal_Handler --
   -------------------------------

   procedure Raise_From_Signal_Handler
     (E : Exception_Id;
      M : Big_String_Ptr)
   is
   begin
      Exception_Data.Set_Exception_C_Msg (E, M);
      Abort_Defer.all;
      Process_Raise_Exception (E => E, From_Signal_Handler => True);
   end Raise_From_Signal_Handler;

   -------------------------
   -- Raise_Program_Error --
   -------------------------

   procedure Raise_Program_Error
     (File : Big_String_Ptr;
      Line : Integer)
   is
   begin
      Raise_With_Location_And_Msg
        (Program_Error_Def'Access, File, Line);
   end Raise_Program_Error;

   -----------------------------
   -- Raise_Program_Error_Msg --
   -----------------------------

   procedure Raise_Program_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr)
   is
   begin
      Raise_With_Location_And_Msg
        (Program_Error_Def'Access, File, Line, Msg);
   end Raise_Program_Error_Msg;

   -------------------------
   -- Raise_Storage_Error --
   -------------------------

   procedure Raise_Storage_Error
     (File : Big_String_Ptr;
      Line : Integer)
   is
   begin
      Raise_With_Location_And_Msg
        (Storage_Error_Def'Access, File, Line);
   end Raise_Storage_Error;

   -----------------------------
   -- Raise_Storage_Error_Msg --
   -----------------------------

   procedure Raise_Storage_Error_Msg
     (File : Big_String_Ptr;
      Line : Integer;
      Msg  : Big_String_Ptr)
   is
   begin
      Raise_With_Location_And_Msg
        (Storage_Error_Def'Access, File, Line, Msg);
   end Raise_Storage_Error_Msg;

   ---------------------------------
   -- Raise_With_Location_And_Msg --
   ---------------------------------

   procedure Raise_With_Location_And_Msg
     (E : Exception_Id;
      F : Big_String_Ptr;
      L : Integer;
      M : Big_String_Ptr := null)
   is
   begin
      Exception_Data.Set_Exception_C_Msg (E, F, L, M);
      Abort_Defer.all;
      Raise_Current_Excep (E);
   end Raise_With_Location_And_Msg;

   --------------------
   -- Raise_With_Msg --
   --------------------

   procedure Raise_With_Msg (E : Exception_Id) is
      Excep : constant EOA := Get_Current_Excep.all;

   begin
      Exception_Propagation.Setup_Exception (Excep, Excep);

      Excep.Exception_Raised := False;
      Excep.Id               := E;
      Excep.Num_Tracebacks   := 0;
      Excep.Cleanup_Flag     := False;
      Excep.Pid              := Local_Partition_ID;
      Abort_Defer.all;
      Raise_Current_Excep (E);
   end Raise_With_Msg;

   --------------------------------------
   -- Calls to Run-Time Check Routines --
   --------------------------------------

   procedure Rcheck_00 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_00'Address));
   end Rcheck_00;

   procedure Rcheck_01 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_01'Address));
   end Rcheck_01;

   procedure Rcheck_02 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_02'Address));
   end Rcheck_02;

   procedure Rcheck_03 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_03'Address));
   end Rcheck_03;

   procedure Rcheck_04 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_04'Address));
   end Rcheck_04;

   procedure Rcheck_05 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_05'Address));
   end Rcheck_05;

   procedure Rcheck_06 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_06'Address));
   end Rcheck_06;

   procedure Rcheck_07 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_07'Address));
   end Rcheck_07;

   procedure Rcheck_08 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_08'Address));
   end Rcheck_08;

   procedure Rcheck_09 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_09'Address));
   end Rcheck_09;

   procedure Rcheck_10 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_10'Address));
   end Rcheck_10;

   procedure Rcheck_11 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Constraint_Error_Msg (File, Line, To_Ptr (Rmsg_11'Address));
   end Rcheck_11;

   procedure Rcheck_12 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_12'Address));
   end Rcheck_12;

   procedure Rcheck_13 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_13'Address));
   end Rcheck_13;

   procedure Rcheck_14 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_14'Address));
   end Rcheck_14;

   procedure Rcheck_15 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_15'Address));
   end Rcheck_15;

   procedure Rcheck_16 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_16'Address));
   end Rcheck_16;

   procedure Rcheck_17 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_17'Address));
   end Rcheck_17;

   procedure Rcheck_18 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_18'Address));
   end Rcheck_18;

   procedure Rcheck_19 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_19'Address));
   end Rcheck_19;

   procedure Rcheck_20 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_20'Address));
   end Rcheck_20;

   procedure Rcheck_21 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_21'Address));
   end Rcheck_21;

   procedure Rcheck_22 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_22'Address));
   end Rcheck_22;

   procedure Rcheck_23 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_23'Address));
   end Rcheck_23;

   procedure Rcheck_24 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Program_Error_Msg (File, Line, To_Ptr (Rmsg_24'Address));
   end Rcheck_24;

   procedure Rcheck_25 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Storage_Error_Msg (File, Line, To_Ptr (Rmsg_25'Address));
   end Rcheck_25;

   procedure Rcheck_26 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Storage_Error_Msg (File, Line, To_Ptr (Rmsg_26'Address));
   end Rcheck_26;

   procedure Rcheck_27 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Storage_Error_Msg (File, Line, To_Ptr (Rmsg_27'Address));
   end Rcheck_27;

   procedure Rcheck_28 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Storage_Error_Msg (File, Line, To_Ptr (Rmsg_28'Address));
   end Rcheck_28;

   procedure Rcheck_29 (File : Big_String_Ptr; Line : Integer) is
   begin
      Raise_Storage_Error_Msg (File, Line, To_Ptr (Rmsg_29'Address));
   end Rcheck_29;

   -------------
   -- Reraise --
   -------------

   procedure Reraise is
      Excep : constant EOA := Get_Current_Excep.all;

   begin
      Abort_Defer.all;
      Exception_Propagation.Setup_Exception (Excep, Excep, Reraised => True);
      Raise_Current_Excep (Excep.Id);
   end Reraise;

   ------------------------
   -- Reraise_Occurrence --
   ------------------------

   procedure Reraise_Occurrence (X : Exception_Occurrence) is
   begin
      if X.Id /= null then
         Abort_Defer.all;
         Exception_Propagation.Setup_Exception
           (X'Unrestricted_Access, Get_Current_Excep.all, Reraised => True);
         Save_Occurrence_No_Private (Get_Current_Excep.all.all, X);
         Raise_Current_Excep (X.Id);
      end if;
   end Reraise_Occurrence;

   -------------------------------
   -- Reraise_Occurrence_Always --
   -------------------------------

   procedure Reraise_Occurrence_Always (X : Exception_Occurrence) is
   begin
      Abort_Defer.all;
      Exception_Propagation.Setup_Exception
        (X'Unrestricted_Access, Get_Current_Excep.all, Reraised => True);
      Save_Occurrence_No_Private (Get_Current_Excep.all.all, X);
      Raise_Current_Excep (X.Id);
   end Reraise_Occurrence_Always;

   ---------------------------------
   -- Reraise_Occurrence_No_Defer --
   ---------------------------------

   procedure Reraise_Occurrence_No_Defer (X : Exception_Occurrence) is
   begin
      Exception_Propagation.Setup_Exception
        (X'Unrestricted_Access, Get_Current_Excep.all, Reraised => True);
      Save_Occurrence_No_Private (Get_Current_Excep.all.all, X);
      Raise_Current_Excep (X.Id);
   end Reraise_Occurrence_No_Defer;

   ---------------------
   -- Save_Occurrence --
   ---------------------

   procedure Save_Occurrence
     (Target : out Exception_Occurrence;
      Source : Exception_Occurrence)
   is
   begin
      Save_Occurrence_No_Private (Target, Source);
   end Save_Occurrence;

   function Save_Occurrence
     (Source : Exception_Occurrence)
      return   EOA
   is
      Target : EOA := new Exception_Occurrence;

   begin
      Save_Occurrence (Target.all, Source);
      return Target;
   end Save_Occurrence;

   --------------------------------
   -- Save_Occurrence_And_Private --
   --------------------------------

   procedure Save_Occurrence_And_Private
     (Target : out Exception_Occurrence;
      Source : Exception_Occurrence)
   is
   begin
      Save_Occurrence_No_Private (Target, Source);
      Target.Private_Data := Source.Private_Data;
   end Save_Occurrence_And_Private;

   --------------------------------
   -- Save_Occurrence_No_Private --
   --------------------------------

   procedure Save_Occurrence_No_Private
     (Target : out Exception_Occurrence;
      Source : Exception_Occurrence)
   is
   begin
      Target.Id             := Source.Id;
      Target.Msg_Length     := Source.Msg_Length;
      Target.Num_Tracebacks := Source.Num_Tracebacks;
      Target.Pid            := Source.Pid;
      Target.Cleanup_Flag   := Source.Cleanup_Flag;

      Target.Msg (1 .. Target.Msg_Length) :=
        Source.Msg (1 .. Target.Msg_Length);

      Target.Tracebacks (1 .. Target.Num_Tracebacks) :=
        Source.Tracebacks (1 .. Target.Num_Tracebacks);
   end Save_Occurrence_No_Private;

   -------------------------
   -- Transfer_Occurrence --
   -------------------------

   procedure Transfer_Occurrence
     (Target : Exception_Occurrence_Access;
      Source : Exception_Occurrence)
   is
   begin
      --  Setup Target as an exception to be propagated in the calling task
      --  (rendezvous-wise), taking care not to clobber the associated private
      --  data.  Target is expected to be a pointer to the calling task's
      --  fixed TSD occurrence, which is very different from Get_Current_Excep
      --  here because this subprogram is called from the called task.

      Exception_Propagation.Setup_Exception (Target, Target);
      Save_Occurrence_No_Private (Target.all, Source);
   end Transfer_Occurrence;

   -------------------
   -- String_To_EId --
   -------------------

   function String_To_EId (S : String) return Exception_Id
     renames Stream_Attributes.String_To_EId;

   ------------------
   -- String_To_EO --
   ------------------

   function String_To_EO (S : String) return Exception_Occurrence
     renames Stream_Attributes.String_To_EO;

   ------------------------------
   -- Raise_Exception_No_Defer --
   ------------------------------

   procedure Raise_Exception_No_Defer
     (E       : Exception_Id;
      Message : String := "")
   is
   begin
      Exception_Data.Set_Exception_Msg (E, Message);

      --  DO NOT CALL Abort_Defer.all; !!!!
      --  why not??? would be nice to have more comments here

      Raise_Current_Excep (E);
   end Raise_Exception_No_Defer;

   ---------------
   -- To_Stderr --
   ---------------

   procedure To_Stderr (C : Character) is

      type int is new Integer;

      procedure put_char_stderr (C : int);
      pragma Import (C, put_char_stderr, "put_char_stderr");

   begin
      put_char_stderr (Character'Pos (C));
   end To_Stderr;

   procedure To_Stderr (S : String) is
   begin
      for J in S'Range loop
         if S (J) /= ASCII.CR then
            To_Stderr (S (J));
         end if;
      end loop;
   end To_Stderr;

   ---------
   -- ZZZ --
   ---------

   --  This dummy procedure gives us the end of the PC range for addresses
   --  within the exception unit itself. We hope that gigi/gcc keeps all the
   --  procedures in their original order!

   procedure ZZZ is
   begin
      <<Start_Of_ZZZ>>
      Code_Address_For_ZZZ := Start_Of_ZZZ'Address;
   end ZZZ;

begin
   --  Allocate the Non-Tasking Machine_State

   Set_Machine_State_Addr_NT (System.Address (Allocate_Machine_State));

   --  Call the AAA/ZZZ routines to setup the code addresses for the
   --  bounds of this unit.

   AAA;
   ZZZ;
end Ada.Exceptions;
