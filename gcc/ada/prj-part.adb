------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--                             P R J . P A R T                              --
--                                                                          --
--                                 B o d y                                  --
--                                                                          --
--          Copyright (C) 2001-2004 Free Software Foundation, Inc.          --
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
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

with Err_Vars; use Err_Vars;
with Namet;    use Namet;
with Opt;      use Opt;
with Osint;    use Osint;
with Output;   use Output;
with Prj.Com;  use Prj.Com;
with Prj.Dect;
with Prj.Err;  use Prj.Err;
with Scans;    use Scans;
with Sinput;   use Sinput;
with Sinput.P; use Sinput.P;
with Snames;
with Table;
with Types;    use Types;

with Ada.Characters.Handling;    use Ada.Characters.Handling;
with Ada.Exceptions;             use Ada.Exceptions;

with GNAT.Directory_Operations;  use GNAT.Directory_Operations;
with GNAT.OS_Lib;                use GNAT.OS_Lib;

with System.HTable;              use System.HTable;

pragma Elaborate_All (GNAT.OS_Lib);

package body Prj.Part is

   Dir_Sep  : Character renames GNAT.OS_Lib.Directory_Separator;

   Project_Path : String_Access;
   --  The project path; initialized during package elaboration.
   --  Contains at least the current working directory.

   Ada_Project_Path : constant String := "ADA_PROJECT_PATH";
   --  Name of the env. variable that contains path name(s) of directories
   --  where project files may reside.

   Prj_Path : constant String_Access := Getenv (Ada_Project_Path);
   --  The path name(s) of directories where project files may reside.
   --  May be empty.

   type Extension_Origin is (None, Extending_Simple, Extending_All);
   --  Type of parameter From_Extended for procedures Parse_Single_Project and
   --  Post_Parse_Context_Clause. Extending_All means that we are parsing the
   --  tree rooted at an extending all project.

   ------------------------------------
   -- Local Packages and Subprograms --
   ------------------------------------

   type With_Id is new Nat;
   No_With : constant With_Id := 0;

   type With_Record is record
      Path         : Name_Id;
      Location     : Source_Ptr;
      Limited_With : Boolean;
      Node         : Project_Node_Id;
      Next         : With_Id;
   end record;
   --  Information about an imported project, to be put in table Withs below

   package Withs is new Table.Table
     (Table_Component_Type => With_Record,
      Table_Index_Type     => With_Id,
      Table_Low_Bound      => 1,
      Table_Initial        => 10,
      Table_Increment      => 50,
      Table_Name           => "Prj.Part.Withs");
   --  Table used to store temporarily paths and locations of imported
   --  projects. These imported projects will be effectively parsed after the
   --  name of the current project has been extablished.

   type Names_And_Id is record
      Path_Name           : Name_Id;
      Canonical_Path_Name : Name_Id;
      Id                  : Project_Node_Id;
   end record;

   package Project_Stack is new Table.Table
     (Table_Component_Type => Names_And_Id,
      Table_Index_Type     => Nat,
      Table_Low_Bound      => 1,
      Table_Initial        => 10,
      Table_Increment      => 50,
      Table_Name           => "Prj.Part.Project_Stack");
   --  This table is used to detect circular dependencies
   --  for imported and extended projects and to get the project ids of
   --  limited imported projects when there is a circularity with at least
   --  one limited imported project file.

   package Virtual_Hash is new Simple_HTable
     (Header_Num => Header_Num,
      Element    => Project_Node_Id,
      No_Element => Empty_Node,
      Key        => Project_Node_Id,
      Hash       => Prj.Tree.Hash,
      Equal      => "=");
   --  Hash table to store the node id of the project for which a virtual
   --  extending project need to be created.

   package Processed_Hash is new Simple_HTable
     (Header_Num => Header_Num,
      Element    => Boolean,
      No_Element => False,
      Key        => Project_Node_Id,
      Hash       => Prj.Tree.Hash,
      Equal      => "=");
   --  Hash table to store the project process when looking for project that
   --  need to have a virtual extending project, to avoid processing the same
   --  project twice.

   procedure Create_Virtual_Extending_Project
     (For_Project  : Project_Node_Id;
      Main_Project : Project_Node_Id);
   --  Create a virtual extending project of For_Project. Main_Project is
   --  the extending all project.

   procedure Look_For_Virtual_Projects_For
     (Proj                : Project_Node_Id;
      Potentially_Virtual : Boolean);
   --  Look for projects that need to have a virtual extending project.
   --  This procedure is recursive. If called with Potentially_Virtual set to
   --  True, then Proj may need an virtual extending project; otherwise it
   --  does not (because it is already extended), but other projects that it
   --  imports may need to be virtually extended.

   procedure Pre_Parse_Context_Clause (Context_Clause : out With_Id);
   --  Parse the context clause of a project.
   --  Store the paths and locations of the imported projects in table Withs.
   --  Does nothing if there is no context clause (if the current
   --  token is not "with" or "limited" followed by "with").

   procedure Post_Parse_Context_Clause
     (Context_Clause    : With_Id;
      Imported_Projects : out Project_Node_Id;
      Project_Directory : Name_Id;
      From_Extended     : Extension_Origin;
      In_Limited        : Boolean);
   --  Parse the imported projects that have been stored in table Withs,
   --  if any. From_Extended is used for the call to Parse_Single_Project
   --  below. When In_Limited is True, the importing path includes at least
   --  one "limited with".

   procedure Parse_Single_Project
     (Project       : out Project_Node_Id;
      Extends_All   : out Boolean;
      Path_Name     : String;
      Extended      : Boolean;
      From_Extended : Extension_Origin;
      In_Limited    : Boolean);
   --  Parse a project file.
   --  Recursive procedure: it calls itself for imported and extended
   --  projects. When From_Extended is not None, if the project has already
   --  been parsed and is an extended project A, return the ultimate
   --  (not extended) project that extends A. When In_Limited is True,
   --  the importing path includes at least one "limited with".

   function Project_Path_Name_Of
     (Project_File_Name : String;
      Directory         : String) return String;
   --  Returns the path name of a project file. Returns an empty string
   --  if project file cannot be found.

   function Immediate_Directory_Of (Path_Name : Name_Id) return Name_Id;
   --  Get the directory of the file with the specified path name.
   --  This includes the directory separator as the last character.
   --  Returns "./" if Path_Name contains no directory separator.

   function Project_Name_From (Path_Name : String) return Name_Id;
   --  Returns the name of the project that corresponds to its path name.
   --  Returns No_Name if the path name is invalid, because the corresponding
   --  project name does not have the syntax of an ada identifier.

   --------------------------------------
   -- Create_Virtual_Extending_Project --
   --------------------------------------

   procedure Create_Virtual_Extending_Project
     (For_Project  : Project_Node_Id;
      Main_Project : Project_Node_Id)
   is

      Virtual_Name : constant String :=
                       Virtual_Prefix &
                         Get_Name_String (Name_Of (For_Project));
      --  The name of the virtual extending project

      Virtual_Name_Id : Name_Id;
      --  Virtual extending project name id

      Virtual_Path_Id : Name_Id;
      --  Fake path name of the virtual extending project. The directory is
      --  the same directory as the extending all project.

      Virtual_Dir_Id  : constant Name_Id :=
                          Immediate_Directory_Of (Path_Name_Of (Main_Project));
      --  The directory of the extending all project

      --  The source of the virtual extending project is something like:

      --  project V$<project name> extends <project path> is

      --     for Source_Dirs use ();

      --  end V$<project name>;

      --  The project directory cannot be specified during parsing; it will be
      --  put directly in the virtual extending project data during processing.

      --  Nodes that made up the virtual extending project

      Virtual_Project         : constant Project_Node_Id :=
                                  Default_Project_Node (N_Project);
      With_Clause             : constant Project_Node_Id :=
                                  Default_Project_Node (N_With_Clause);
      Project_Declaration     : constant Project_Node_Id :=
                                  Default_Project_Node (N_Project_Declaration);
      Source_Dirs_Declaration : constant Project_Node_Id :=
                                  Default_Project_Node (N_Declarative_Item);
      Source_Dirs_Attribute   : constant Project_Node_Id :=
                                  Default_Project_Node
                                    (N_Attribute_Declaration, List);
      Source_Dirs_Expression  : constant Project_Node_Id :=
                                  Default_Project_Node (N_Expression, List);
      Source_Dirs_Term        : constant Project_Node_Id :=
                                  Default_Project_Node (N_Term, List);
      Source_Dirs_List        : constant Project_Node_Id :=
                                  Default_Project_Node
                                    (N_Literal_String_List, List);

   begin
      --  Get the virtual name id

      Name_Len := Virtual_Name'Length;
      Name_Buffer (1 .. Name_Len) := Virtual_Name;
      Virtual_Name_Id := Name_Find;

      --  Get the virtual path name

      Get_Name_String (Path_Name_Of (Main_Project));

      while Name_Len > 0
        and then Name_Buffer (Name_Len) /= Directory_Separator
        and then Name_Buffer (Name_Len) /= '/'
      loop
         Name_Len := Name_Len - 1;
      end loop;

      Name_Buffer (Name_Len + 1 .. Name_Len + Virtual_Name'Length) :=
        Virtual_Name;
      Name_Len := Name_Len + Virtual_Name'Length;
      Virtual_Path_Id := Name_Find;

      --  With clause

      Set_Name_Of (With_Clause, Virtual_Name_Id);
      Set_Path_Name_Of (With_Clause, Virtual_Path_Id);
      Set_Project_Node_Of (With_Clause, Virtual_Project);
      Set_Next_With_Clause_Of
        (With_Clause, First_With_Clause_Of (Main_Project));
      Set_First_With_Clause_Of (Main_Project, With_Clause);

      --  Virtual project node

      Set_Name_Of (Virtual_Project, Virtual_Name_Id);
      Set_Path_Name_Of (Virtual_Project, Virtual_Path_Id);
      Set_Location_Of (Virtual_Project, Location_Of (Main_Project));
      Set_Directory_Of (Virtual_Project, Virtual_Dir_Id);
      Set_Project_Declaration_Of (Virtual_Project, Project_Declaration);
      Set_Extended_Project_Path_Of
        (Virtual_Project, Path_Name_Of (For_Project));

      --  Project declaration

      Set_First_Declarative_Item_Of
        (Project_Declaration, Source_Dirs_Declaration);
      Set_Extended_Project_Of (Project_Declaration, For_Project);

      --  Source_Dirs declaration

      Set_Current_Item_Node (Source_Dirs_Declaration, Source_Dirs_Attribute);

      --  Source_Dirs attribute

      Set_Name_Of (Source_Dirs_Attribute, Snames.Name_Source_Dirs);
      Set_Expression_Of (Source_Dirs_Attribute, Source_Dirs_Expression);

      --  Source_Dirs expression

      Set_First_Term (Source_Dirs_Expression, Source_Dirs_Term);

      --  Source_Dirs term

      Set_Current_Term (Source_Dirs_Term, Source_Dirs_List);

      --  Source_Dirs empty list: nothing to do

   end Create_Virtual_Extending_Project;

   ----------------------------
   -- Immediate_Directory_Of --
   ----------------------------

   function Immediate_Directory_Of (Path_Name : Name_Id) return Name_Id is
   begin
      Get_Name_String (Path_Name);

      for Index in reverse 1 .. Name_Len loop
         if Name_Buffer (Index) = '/'
           or else Name_Buffer (Index) = Dir_Sep
         then
            --  Remove all chars after last directory separator from name

            if Index > 1 then
               Name_Len := Index - 1;

            else
               Name_Len := Index;
            end if;

            return Name_Find;
         end if;
      end loop;

      --  There is no directory separator in name. Return "./" or ".\"

      Name_Len := 2;
      Name_Buffer (1) := '.';
      Name_Buffer (2) := Dir_Sep;
      return Name_Find;
   end Immediate_Directory_Of;

   -----------------------------------
   -- Look_For_Virtual_Projects_For --
   -----------------------------------

   procedure Look_For_Virtual_Projects_For
     (Proj                : Project_Node_Id;
      Potentially_Virtual : Boolean)

   is
      Declaration : Project_Node_Id := Empty_Node;
      --  Node for the project declaration of Proj

      With_Clause : Project_Node_Id := Empty_Node;
      --  Node for a with clause of Proj

      Imported    : Project_Node_Id := Empty_Node;
      --  Node for a project imported by Proj

      Extended    : Project_Node_Id := Empty_Node;
      --  Node for the eventual project extended by Proj

   begin
      --  Nothing to do if Proj is not defined or if it has already been
      --  processed.

      if Proj /= Empty_Node and then not Processed_Hash.Get (Proj) then
         --  Make sure the project will not be processed again

         Processed_Hash.Set (Proj, True);

         Declaration := Project_Declaration_Of (Proj);

         if Declaration /= Empty_Node then
            Extended := Extended_Project_Of (Declaration);
         end if;

         --  If this is a project that may need a virtual extending project
         --  and it is not itself an extending project, put it in the list.

         if Potentially_Virtual and then Extended = Empty_Node then
            Virtual_Hash.Set (Proj, Proj);
         end if;

         --  Now check the projects it imports

         With_Clause := First_With_Clause_Of (Proj);

         while With_Clause /= Empty_Node loop
            Imported := Project_Node_Of (With_Clause);

            if Imported /= Empty_Node then
               Look_For_Virtual_Projects_For
                 (Imported, Potentially_Virtual => True);
            end if;

            With_Clause := Next_With_Clause_Of (With_Clause);
         end loop;

         --  Check also the eventual project extended by Proj. As this project
         --  is already extended, call recursively with Potentially_Virtual
         --  being False.

         Look_For_Virtual_Projects_For
           (Extended, Potentially_Virtual => False);
      end if;
   end Look_For_Virtual_Projects_For;

   -----------
   -- Parse --
   -----------

   procedure Parse
     (Project                : out Project_Node_Id;
      Project_File_Name      : String;
      Always_Errout_Finalize : Boolean;
      Packages_To_Check      : String_List_Access := All_Packages;
      Store_Comments         : Boolean := False)
   is
      Current_Directory : constant String := Get_Current_Dir;
      Dummy : Boolean;

   begin
      --  Save the Packages_To_Check in Prj, so that it is visible from
      --  Prj.Dect.

      Current_Packages_To_Check := Packages_To_Check;

      Project := Empty_Node;

      if Current_Verbosity >= Medium then
         Write_Str ("ADA_PROJECT_PATH=""");
         Write_Str (Project_Path.all);
         Write_Line ("""");
      end if;

      declare
         Path_Name : constant String :=
                       Project_Path_Name_Of (Project_File_Name,
                                             Directory   => Current_Directory);

      begin
         Prj.Err.Initialize;
         Prj.Err.Scanner.Set_Comment_As_Token (Store_Comments);
         Prj.Err.Scanner.Set_End_Of_Line_As_Token (Store_Comments);

         --  Parse the main project file

         if Path_Name = "" then
            Prj.Com.Fail
              ("project file """, Project_File_Name, """ not found");
            Project := Empty_Node;
            return;
         end if;

         Parse_Single_Project
           (Project       => Project,
            Extends_All   => Dummy,
            Path_Name     => Path_Name,
            Extended      => False,
            From_Extended => None,
            In_Limited    => False);

         --  If Project is an extending-all project, create the eventual
         --  virtual extending projects and check that there are no illegally
         --  imported projects.

         if Project /= Empty_Node and then Is_Extending_All (Project) then
            --  First look for projects that potentially need a virtual
            --  extending project.

            Virtual_Hash.Reset;
            Processed_Hash.Reset;

            --  Mark the extending all project as processed, to avoid checking
            --  the imported projects in case of a "limited with" on this
            --  extending all project.

            Processed_Hash.Set (Project, True);

            declare
               Declaration : constant Project_Node_Id :=
                 Project_Declaration_Of (Project);
            begin
               Look_For_Virtual_Projects_For
                 (Extended_Project_Of (Declaration),
                  Potentially_Virtual => False);
            end;

            --  Now, check the projects directly imported by the main project.
            --  Remove from the potentially virtual any project extended by one
            --  of these imported projects. For non extending imported
            --  projects, check that they do not belong to the project tree of
            --  the project being "extended-all" by the main project.

            declare
               With_Clause : Project_Node_Id :=
                 First_With_Clause_Of (Project);
               Imported    : Project_Node_Id := Empty_Node;
               Declaration : Project_Node_Id := Empty_Node;

            begin
               while With_Clause /= Empty_Node loop
                  Imported := Project_Node_Of (With_Clause);

                  if Imported /= Empty_Node then
                     Declaration := Project_Declaration_Of (Imported);

                     if Extended_Project_Of (Declaration) /= Empty_Node then
                        loop
                           Imported := Extended_Project_Of (Declaration);
                           exit when Imported = Empty_Node;
                           Virtual_Hash.Remove (Imported);
                           Declaration := Project_Declaration_Of (Imported);
                        end loop;

                     elsif Virtual_Hash.Get (Imported) /= Empty_Node then
                        Error_Msg
                          ("this project cannot be imported directly",
                           Location_Of (With_Clause));
                     end if;

                  end if;

                  With_Clause := Next_With_Clause_Of (With_Clause);
               end loop;
            end;

            --  Now create all the virtual extending projects

            declare
               Proj : Project_Node_Id := Virtual_Hash.Get_First;
            begin
               while Proj /= Empty_Node loop
                  Create_Virtual_Extending_Project (Proj, Project);
                  Proj := Virtual_Hash.Get_Next;
               end loop;
            end;
         end if;

         --  If there were any kind of error during the parsing, serious
         --  or not, then the parsing fails.

         if Err_Vars.Total_Errors_Detected > 0 then
            Project := Empty_Node;
         end if;

         if Project = Empty_Node or else Always_Errout_Finalize then
            Prj.Err.Finalize;
         end if;
      end;

   exception
      when X : others =>

         --  Internal error

         Write_Line (Exception_Information (X));
         Write_Str  ("Exception ");
         Write_Str  (Exception_Name (X));
         Write_Line (" raised, while processing project file");
         Project := Empty_Node;
   end Parse;

   ------------------------------
   -- Pre_Parse_Context_Clause --
   ------------------------------

   procedure Pre_Parse_Context_Clause (Context_Clause : out With_Id) is
      Current_With_Clause    : With_Id := No_With;
      Limited_With           : Boolean         := False;

      Current_With : With_Record;

      Current_With_Node : Project_Node_Id := Empty_Node;

   begin
      --  Assume no context clause

      Context_Clause := No_With;
      With_Loop :

      --  If Token is not WITH or LIMITED, there is no context clause,
      --  or we have exhausted the with clauses.

      while Token = Tok_With or else Token = Tok_Limited loop
         Current_With_Node := Default_Project_Node (Of_Kind => N_With_Clause);
         Limited_With := Token = Tok_Limited;

         if Limited_With then
            Scan;  --  scan past LIMITED
            Expect (Tok_With, "WITH");
            exit With_Loop when Token /= Tok_With;
         end if;

         Comma_Loop :
         loop
            Scan; -- scan past WITH or ","

            Expect (Tok_String_Literal, "literal string");

            if Token /= Tok_String_Literal then
               return;
            end if;

            --  Store path and location in table Withs

            Current_With :=
              (Path         => Token_Name,
               Location     => Token_Ptr,
               Limited_With => Limited_With,
               Node         => Current_With_Node,
               Next         => No_With);

            Withs.Increment_Last;
            Withs.Table (Withs.Last) := Current_With;

            if Current_With_Clause = No_With then
               Context_Clause := Withs.Last;

            else
               Withs.Table (Current_With_Clause).Next := Withs.Last;
            end if;

            Current_With_Clause := Withs.Last;

            Scan;

            if Token = Tok_Semicolon then
               Set_End_Of_Line (Current_With_Node);
               Set_Previous_Line_Node (Current_With_Node);

               --  End of (possibly multiple) with clause;

               Scan; -- scan past the semicolon.
               exit Comma_Loop;

            elsif Token /= Tok_Comma then
               Error_Msg ("expected comma or semi colon", Token_Ptr);
               exit Comma_Loop;
            end if;

            Current_With_Node :=
              Default_Project_Node (Of_Kind => N_With_Clause);
         end loop Comma_Loop;
      end loop With_Loop;
   end Pre_Parse_Context_Clause;


   -------------------------------
   -- Post_Parse_Context_Clause --
   -------------------------------

   procedure Post_Parse_Context_Clause
     (Context_Clause    : With_Id;
      Imported_Projects : out Project_Node_Id;
      Project_Directory : Name_Id;
      From_Extended     : Extension_Origin;
      In_Limited        : Boolean)
   is
      Current_With_Clause : With_Id := Context_Clause;

      Current_Project  : Project_Node_Id := Empty_Node;
      Previous_Project : Project_Node_Id := Empty_Node;
      Next_Project     : Project_Node_Id := Empty_Node;

      Project_Directory_Path : constant String :=
                                 Get_Name_String (Project_Directory);

      Current_With : With_Record;
      Limited_With : Boolean := False;
      Extends_All  : Boolean := False;

   begin
      Imported_Projects := Empty_Node;

      while Current_With_Clause /= No_With loop
         Current_With := Withs.Table (Current_With_Clause);
         Current_With_Clause := Current_With.Next;

         Limited_With := In_Limited or Current_With.Limited_With;

         declare
            Original_Path : constant String :=
                                 Get_Name_String (Current_With.Path);

            Imported_Path_Name : constant String :=
                                   Project_Path_Name_Of
                                     (Original_Path,
                                      Project_Directory_Path);

            Withed_Project : Project_Node_Id := Empty_Node;

         begin
            if Imported_Path_Name = "" then

               --  The project file cannot be found

               Error_Msg_Name_1 := Current_With.Path;

               Error_Msg ("unknown project file: {", Current_With.Location);

               --  If this is not imported by the main project file,
               --  display the import path.

               if Project_Stack.Last > 1 then
                  for Index in reverse 1 .. Project_Stack.Last loop
                     Error_Msg_Name_1 := Project_Stack.Table (Index).Path_Name;
                     Error_Msg ("\imported by {", Current_With.Location);
                  end loop;
               end if;

            else
               --  New with clause

               Previous_Project := Current_Project;

               if Current_Project = Empty_Node then

                  --  First with clause of the context clause

                  Current_Project := Current_With.Node;
                  Imported_Projects := Current_Project;

               else
                  Next_Project := Current_With.Node;
                  Set_Next_With_Clause_Of (Current_Project, Next_Project);
                  Current_Project := Next_Project;
               end if;

               Set_String_Value_Of
                 (Current_Project, Current_With.Path);
               Set_Location_Of (Current_Project, Current_With.Location);

               --  If this is a "limited with", check if we have
               --  a circularity; if we have one, get the project id
               --  of the limited imported project file, and don't
               --  parse it.

               if Limited_With and then Project_Stack.Last > 1 then
                  declare
                     Normed : constant String :=
                                Normalize_Pathname (Imported_Path_Name);
                     Canonical_Path_Name : Name_Id;

                  begin
                     Name_Len := Normed'Length;
                     Name_Buffer (1 .. Name_Len) := Normed;
                     Canonical_Case_File_Name (Name_Buffer (1 .. Name_Len));
                     Canonical_Path_Name := Name_Find;

                     for Index in 1 .. Project_Stack.Last loop
                        if Project_Stack.Table (Index).Canonical_Path_Name =
                             Canonical_Path_Name
                        then
                           --  We have found the limited imported project,
                           --  get its project id, and do not parse it.

                           Withed_Project := Project_Stack.Table (Index).Id;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;

               --  Parse the imported project, if its project id is unknown

               if Withed_Project = Empty_Node then
                  Parse_Single_Project
                    (Project       => Withed_Project,
                     Extends_All   => Extends_All,
                     Path_Name     => Imported_Path_Name,
                     Extended      => False,
                     From_Extended => From_Extended,
                     In_Limited    => Limited_With);

               else
                  Extends_All := Is_Extending_All (Withed_Project);
               end if;

               if Withed_Project = Empty_Node then
                  --  If parsing was not successful, remove the
                  --  context clause.

                  Current_Project := Previous_Project;

                  if Current_Project = Empty_Node then
                     Imported_Projects := Empty_Node;

                  else
                     Set_Next_With_Clause_Of
                       (Current_Project, Empty_Node);
                  end if;
               else
                  --  If parsing was successful, record project name
                  --  and path name in with clause

                  Set_Project_Node_Of
                    (Node         => Current_Project,
                     To           => Withed_Project,
                     Limited_With => Limited_With);
                  Set_Name_Of (Current_Project, Name_Of (Withed_Project));
                  Name_Len := Imported_Path_Name'Length;
                  Name_Buffer (1 .. Name_Len) := Imported_Path_Name;
                  Set_Path_Name_Of (Current_Project, Name_Find);

                  if Extends_All then
                     Set_Is_Extending_All (Current_Project);
                  end if;
               end if;
            end if;
         end;
      end loop;
   end Post_Parse_Context_Clause;

   --------------------------
   -- Parse_Single_Project --
   --------------------------

   procedure Parse_Single_Project
     (Project       : out Project_Node_Id;
      Extends_All   : out Boolean;
      Path_Name     : String;
      Extended      : Boolean;
      From_Extended : Extension_Origin;
      In_Limited    : Boolean)
   is
      Normed_Path_Name    : Name_Id;
      Canonical_Path_Name : Name_Id;
      Project_Directory   : Name_Id;
      Project_Scan_State  : Saved_Project_Scan_State;
      Source_Index        : Source_File_Index;

      Extending : Boolean := False;

      Extended_Project    : Project_Node_Id := Empty_Node;

      A_Project_Name_And_Node : Tree_Private_Part.Project_Name_And_Node :=
                                  Tree_Private_Part.Projects_Htable.Get_First;

      Name_From_Path : constant Name_Id := Project_Name_From (Path_Name);

      Name_Of_Project : Name_Id := No_Name;

      First_With : With_Id;

      use Tree_Private_Part;

      Project_Comment_State : Tree.Comment_State;

   begin
      Extends_All := False;

      declare
         Normed_Path    : constant String := Normalize_Pathname
                            (Path_Name, Resolve_Links => False,
                             Case_Sensitive           => True);
         Canonical_Path : constant String := Normalize_Pathname
                            (Normed_Path, Resolve_Links => True,
                             Case_Sensitive             => False);

      begin
         Name_Len := Normed_Path'Length;
         Name_Buffer (1 .. Name_Len) := Normed_Path;
         Normed_Path_Name := Name_Find;
         Name_Len := Canonical_Path'Length;
         Name_Buffer (1 .. Name_Len) := Canonical_Path;
         Canonical_Path_Name := Name_Find;
      end;

      --  Check for a circular dependency

      for Index in 1 .. Project_Stack.Last loop
         if Canonical_Path_Name =
              Project_Stack.Table (Index).Canonical_Path_Name
         then
            Error_Msg ("circular dependency detected", Token_Ptr);
            Error_Msg_Name_1 := Normed_Path_Name;
            Error_Msg ("\  { is imported by", Token_Ptr);

            for Current in reverse 1 .. Project_Stack.Last loop
               Error_Msg_Name_1 := Project_Stack.Table (Current).Path_Name;

               if Project_Stack.Table (Current).Canonical_Path_Name /=
                    Canonical_Path_Name
               then
                  Error_Msg
                    ("\  { which itself is imported by", Token_Ptr);

               else
                  Error_Msg ("\  {", Token_Ptr);
                  exit;
               end if;
            end loop;

            Project := Empty_Node;
            return;
         end if;
      end loop;

      --  Put the new path name on the stack

      Project_Stack.Increment_Last;
      Project_Stack.Table (Project_Stack.Last).Path_Name := Normed_Path_Name;
      Project_Stack.Table (Project_Stack.Last).Canonical_Path_Name :=
        Canonical_Path_Name;

      --  Check if the project file has already been parsed.

      while
        A_Project_Name_And_Node /= Tree_Private_Part.No_Project_Name_And_Node
      loop
         declare
            Path_Id : Name_Id := Path_Name_Of (A_Project_Name_And_Node.Node);

         begin
            if Path_Id /= No_Name then
               Get_Name_String (Path_Id);
               Canonical_Case_File_Name (Name_Buffer (1 .. Name_Len));
               Path_Id := Name_Find;
            end if;

            if Path_Id = Canonical_Path_Name then
               if Extended then

                  if A_Project_Name_And_Node.Extended then
                     Error_Msg
                       ("cannot extend the same project file several times",
                        Token_Ptr);

                  else
                     Error_Msg
                       ("cannot extend an already imported project file",
                        Token_Ptr);
                  end if;

               elsif A_Project_Name_And_Node.Extended then
                  Extends_All :=
                    Is_Extending_All (A_Project_Name_And_Node.Node);

                  --  If the imported project is an extended project A,
                  --  and we are in an extended project, replace A with the
                  --  ultimate project extending A.

                  if From_Extended /= None then
                     declare
                        Decl : Project_Node_Id :=
                                 Project_Declaration_Of
                                   (A_Project_Name_And_Node.Node);

                        Prj : Project_Node_Id :=
                                Extending_Project_Of (Decl);

                     begin
                        loop
                           Decl := Project_Declaration_Of (Prj);
                           exit when Extending_Project_Of (Decl) = Empty_Node;
                           Prj := Extending_Project_Of (Decl);
                        end loop;

                        A_Project_Name_And_Node.Node := Prj;
                     end;
                  else
                     Error_Msg
                       ("cannot import an already extended project file",
                        Token_Ptr);
                  end if;
               end if;

               Project := A_Project_Name_And_Node.Node;
               Project_Stack.Decrement_Last;
               return;
            end if;
         end;

         A_Project_Name_And_Node := Tree_Private_Part.Projects_Htable.Get_Next;
      end loop;

      --  We never encountered this project file
      --  Save the scan state, load the project file and start to scan it.

      Save_Project_Scan_State (Project_Scan_State);
      Source_Index := Load_Project_File (Path_Name);
      Tree.Save (Project_Comment_State);

      --  If we cannot find it, we stop

      if Source_Index = No_Source_File then
         Project := Empty_Node;
         Project_Stack.Decrement_Last;
         return;
      end if;

      Prj.Err.Scanner.Initialize_Scanner (Types.No_Unit, Source_Index);
      Tree.Reset_State;
      Scan;

      if Name_From_Path = No_Name then

         --  The project file name is not correct (no or bad extension,
         --  or not following Ada identifier's syntax).

         Error_Msg_Name_1 := Canonical_Path_Name;
         Error_Msg ("?{ is not a valid path name for a project file",
                    Token_Ptr);
      end if;

      if Current_Verbosity >= Medium then
         Write_Str  ("Parsing """);
         Write_Str  (Path_Name);
         Write_Char ('"');
         Write_Eol;
      end if;

      --  Is there any imported project?

      Pre_Parse_Context_Clause (First_With);

      Project_Directory := Immediate_Directory_Of (Normed_Path_Name);
      Project := Default_Project_Node (Of_Kind => N_Project);
      Project_Stack.Table (Project_Stack.Last).Id := Project;
      Set_Directory_Of (Project, Project_Directory);
      Set_Path_Name_Of (Project, Normed_Path_Name);
      Set_Location_Of (Project, Token_Ptr);

      Expect (Tok_Project, "PROJECT");

      --  Mark location of PROJECT token if present

      if Token = Tok_Project then
         Set_Location_Of (Project, Token_Ptr);
         Scan; -- scan past project
      end if;

      --  Clear the Buffer

      Buffer_Last := 0;

      loop
         Expect (Tok_Identifier, "identifier");

         --  If the token is not an identifier, clear the buffer before
         --  exiting to indicate that the name of the project is ill-formed.

         if Token /= Tok_Identifier then
            Buffer_Last := 0;
            exit;
         end if;

         --  Add the identifier name to the buffer

         Get_Name_String (Token_Name);
         Add_To_Buffer (Name_Buffer (1 .. Name_Len));

         --  Scan past the identifier

         Scan;

         --  If we have a dot, add a dot the the Buffer and look for the next
         --  identifier.

         exit when Token /= Tok_Dot;
         Add_To_Buffer (".");

         --  Scan past the dot

         Scan;
      end loop;

      --  See if this is an extending project

      if Token = Tok_Extends then

         --  Make sure that gnatmake will use mapping files

         Create_Mapping_File := True;

         --  We are extending another project

         Extending := True;

         Scan; -- scan past EXTENDS

         if Token = Tok_All then
            Extends_All := True;
            Set_Is_Extending_All (Project);
            Scan; --  scan past ALL
         end if;
      end if;

      --  If the name is well formed, Buffer_Last is > 0

      if Buffer_Last > 0 then

         --  The Buffer contains the name of the project

         Name_Len := Buffer_Last;
         Name_Buffer (1 .. Name_Len) := Buffer (1 .. Buffer_Last);
         Name_Of_Project := Name_Find;
         Set_Name_Of (Project, Name_Of_Project);

         --  To get expected name of the project file, replace dots by dashes

         Name_Len := Buffer_Last;
         Name_Buffer (1 .. Name_Len) := Buffer (1 .. Buffer_Last);

         for Index in 1 .. Name_Len loop
            if Name_Buffer (Index) = '.' then
               Name_Buffer (Index) := '-';
            end if;
         end loop;

         Canonical_Case_File_Name (Name_Buffer (1 .. Name_Len));

         declare
            Expected_Name : constant Name_Id := Name_Find;

         begin
            --  Output a warning if the actual name is not the expected name

            if Name_From_Path /= No_Name
              and then Expected_Name /= Name_From_Path
            then
               Error_Msg_Name_1 := Expected_Name;
               Error_Msg ("?file name does not match unit name, " &
                          "should be `{" & Project_File_Extension & "`",
                          Token_Ptr);
            end if;
         end;

         declare
            Imported_Projects : Project_Node_Id := Empty_Node;
            From_Ext : Extension_Origin := None;

         begin
            --  Extending_All is always propagated

            if From_Extended = Extending_All or else Extends_All then
               From_Ext := Extending_All;

            --  Otherwise, From_Extended is set to Extending_Single if the
            --  current project is an extending project.

            elsif Extended then
               From_Ext := Extending_Simple;
            end if;

            Post_Parse_Context_Clause
              (Context_Clause    => First_With,
               Imported_Projects => Imported_Projects,
               Project_Directory => Project_Directory,
               From_Extended     => From_Ext,
               In_Limited        => In_Limited);
            Set_First_With_Clause_Of (Project, Imported_Projects);
         end;

         declare
            Name_And_Node : Tree_Private_Part.Project_Name_And_Node :=
                              Tree_Private_Part.Projects_Htable.Get_First;
            Project_Name : Name_Id := Name_And_Node.Name;

         begin
            --  Check if we already have a project with this name

            while Project_Name /= No_Name
              and then Project_Name /= Name_Of_Project
            loop
               Name_And_Node := Tree_Private_Part.Projects_Htable.Get_Next;
               Project_Name := Name_And_Node.Name;
            end loop;

            --  Report an error if we already have a project with this name

            if Project_Name /= No_Name then
               Error_Msg_Name_1 := Project_Name;
               Error_Msg ("duplicate project name {", Location_Of (Project));
               Error_Msg_Name_1 := Path_Name_Of (Name_And_Node.Node);
               Error_Msg ("\already in {", Location_Of (Project));

            else
               --  Otherwise, add the name of the project to the hash table, so
               --  that we can check that no other subsequent project will have
               --  the same name.

               Tree_Private_Part.Projects_Htable.Set
                 (K => Name_Of_Project,
                  E => (Name     => Name_Of_Project,
                        Node     => Project,
                        Extended => Extended));
            end if;
         end;

      end if;

      if Extending then
         Expect (Tok_String_Literal, "literal string");

         if Token = Tok_String_Literal then
            Set_Extended_Project_Path_Of (Project, Token_Name);

            declare
               Original_Path_Name : constant String :=
                                      Get_Name_String (Token_Name);

               Extended_Project_Path_Name : constant String :=
                                              Project_Path_Name_Of
                                                (Original_Path_Name,
                                                   Get_Name_String
                                                     (Project_Directory));

            begin
               if Extended_Project_Path_Name = "" then

                  --  We could not find the project file to extend

                  Error_Msg_Name_1 := Token_Name;

                  Error_Msg ("unknown project file: {", Token_Ptr);

                  --  If we are not in the main project file, display the
                  --  import path.

                  if Project_Stack.Last > 1 then
                     Error_Msg_Name_1 :=
                       Project_Stack.Table (Project_Stack.Last).Path_Name;
                     Error_Msg ("\extended by {", Token_Ptr);

                     for Index in reverse 1 .. Project_Stack.Last - 1 loop
                        Error_Msg_Name_1 :=
                          Project_Stack.Table (Index).Path_Name;
                        Error_Msg ("\imported by {", Token_Ptr);
                     end loop;
                  end if;

               else
                  declare
                     From_Ext : Extension_Origin := None;

                  begin
                     if From_Extended = Extending_All or else Extends_All then
                        From_Ext := Extending_All;
                     end if;

                     Parse_Single_Project
                       (Project       => Extended_Project,
                        Extends_All   => Extends_All,
                        Path_Name     => Extended_Project_Path_Name,
                        Extended      => True,
                        From_Extended => From_Ext,
                        In_Limited    => In_Limited);
                  end;

                  --  A project that extends an extending-all project is also
                  --  an extending-all project.

                  if Is_Extending_All (Extended_Project) then
                     Set_Is_Extending_All (Project);
                  end if;
               end if;
            end;

            Scan; -- scan past the extended project path
         end if;
      end if;

      --  Check that a non extending-all project does not import an
      --  extending-all project.

      if not Is_Extending_All (Project) then
         declare
            With_Clause : Project_Node_Id := First_With_Clause_Of (Project);
            Imported    : Project_Node_Id := Empty_Node;

         begin
            With_Clause_Loop :
            while With_Clause /= Empty_Node loop
               Imported := Project_Node_Of (With_Clause);

               if Is_Extending_All (With_Clause) then
                  Error_Msg_Name_1 := Name_Of (Imported);
                  Error_Msg ("cannot import extending-all project {",
                             Token_Ptr);
                  exit With_Clause_Loop;
               end if;

               With_Clause := Next_With_Clause_Of (With_Clause);
            end loop With_Clause_Loop;
         end;
      end if;

      --  Check that a project with a name including a dot either imports
      --  or extends the project whose name precedes the last dot.

      if Name_Of_Project /= No_Name then
         Get_Name_String (Name_Of_Project);

      else
         Name_Len := 0;
      end if;

      --  Look for the last dot

      while Name_Len > 0 and then Name_Buffer (Name_Len) /= '.' loop
         Name_Len := Name_Len - 1;
      end loop;

      --  If a dot was find, check if the parent project is imported
      --  or extended.

      if Name_Len > 0 then
         Name_Len := Name_Len - 1;

         declare
            Parent_Name  : constant Name_Id := Name_Find;
            Parent_Found : Boolean := False;
            With_Clause  : Project_Node_Id := First_With_Clause_Of (Project);

         begin
            --  If there is an extended project, check its name

            if Extended_Project /= Empty_Node then
               Parent_Found := Name_Of (Extended_Project) = Parent_Name;
            end if;

            --  If the parent project is not the extended project,
            --  check each imported project until we find the parent project.

            while not Parent_Found and then With_Clause /= Empty_Node loop
               Parent_Found := Name_Of (Project_Node_Of (With_Clause))
                 = Parent_Name;
               With_Clause := Next_With_Clause_Of (With_Clause);
            end loop;

            --  If the parent project was not found, report an error

            if not Parent_Found then
               Error_Msg_Name_1 := Name_Of_Project;
               Error_Msg_Name_2 := Parent_Name;
               Error_Msg ("project { does not import or extend project {",
                          Location_Of (Project));
            end if;
         end;
      end if;

      Expect (Tok_Is, "IS");
      Set_End_Of_Line (Project);
      Set_Previous_Line_Node (Project);
      Set_Next_End_Node (Project);

      declare
         Project_Declaration : Project_Node_Id := Empty_Node;

      begin
         --  No need to Scan past "is", Prj.Dect.Parse will do it.

         Prj.Dect.Parse
           (Declarations    => Project_Declaration,
            Current_Project => Project,
            Extends         => Extended_Project);
         Set_Project_Declaration_Of (Project, Project_Declaration);

         if Extended_Project /= Empty_Node then
            Set_Extending_Project_Of
              (Project_Declaration_Of (Extended_Project), To => Project);
         end if;
      end;

      Expect (Tok_End, "END");
      Remove_Next_End_Node;

      --  Skip "end" if present

      if Token = Tok_End then
         Scan;
      end if;

      --  Clear the Buffer

      Buffer_Last := 0;

      --  Store the name following "end" in the Buffer. The name may be made of
      --  several simple names.

      loop
         Expect (Tok_Identifier, "identifier");

         --  If we don't have an identifier, clear the buffer before exiting to
         --  avoid checking the name.

         if Token /= Tok_Identifier then
            Buffer_Last := 0;
            exit;
         end if;

         --  Add the identifier to the Buffer
         Get_Name_String (Token_Name);
         Add_To_Buffer (Name_Buffer (1 .. Name_Len));

         --  Scan past the identifier

         Scan;
         exit when Token /= Tok_Dot;
         Add_To_Buffer (".");
         Scan;
      end loop;

      --  If we have a valid name, check if it is the name of the project

      if Name_Of_Project /= No_Name and then Buffer_Last > 0 then
         if To_Lower (Buffer (1 .. Buffer_Last)) /=
            Get_Name_String (Name_Of (Project))
         then
            --  Invalid name: report an error

            Error_Msg ("Expected """ &
                       Get_Name_String (Name_Of (Project)) & """",
                       Token_Ptr);
         end if;
      end if;

      Expect (Tok_Semicolon, "`;`");

      --  Check that there is no more text following the end of the project
      --  source.

      if Token = Tok_Semicolon then
         Set_Previous_End_Node (Project);
         Scan;

         if Token /= Tok_EOF then
            Error_Msg
              ("Unexpected text following end of project", Token_Ptr);
         end if;
      end if;

      --  Restore the scan state, in case we are not the main project

      Restore_Project_Scan_State (Project_Scan_State);

      --  And remove the project from the project stack

      Project_Stack.Decrement_Last;

      --  Indicate if there are unkept comments

      Tree.Set_Project_File_Includes_Unkept_Comments
        (Node => Project, To => Tree.There_Are_Unkept_Comments);

      --  And restore the comment state that was saved

      Tree.Restore (Project_Comment_State);
   end Parse_Single_Project;

   -----------------------
   -- Project_Name_From --
   -----------------------

   function Project_Name_From (Path_Name : String) return Name_Id is
      Canonical : String (1 .. Path_Name'Length) := Path_Name;
      First : Natural := Canonical'Last;
      Last  : Natural := First;
      Index : Positive;

   begin
      if Current_Verbosity = High then
         Write_Str ("Project_Name_From (""");
         Write_Str (Canonical);
         Write_Line (""")");
      end if;

      --  If the path name is empty, return No_Name to indicate failure

      if First = 0 then
         return No_Name;
      end if;

      Canonical_Case_File_Name (Canonical);

      --  Look for the last dot in the path name

      while First > 0
        and then
        Canonical (First) /= '.'
      loop
         First := First - 1;
      end loop;

      --  If we have a dot, check that it is followed by the correct extension

      if First > 0 and then Canonical (First) = '.' then
         if Canonical (First .. Last) = Project_File_Extension
           and then First /= 1
         then
            --  Look for the last directory separator, if any

            First := First - 1;
            Last := First;

            while First > 0
              and then Canonical (First) /= '/'
              and then Canonical (First) /= Dir_Sep
            loop
               First := First - 1;
            end loop;

         else
            --  Not the correct extension, return No_Name to indicate failure

            return No_Name;
         end if;

      --  If no dot in the path name, return No_Name to indicate failure

      else
         return No_Name;
      end if;

      First := First + 1;

      --  If the extension is the file name, return No_Name to indicate failure

      if First > Last then
         return No_Name;
      end if;

      --  Put the name in lower case into Name_Buffer

      Name_Len := Last - First + 1;
      Name_Buffer (1 .. Name_Len) := To_Lower (Canonical (First .. Last));

      Index := 1;

      --  Check if it is a well formed project name. Return No_Name if it is
      --  ill formed.

      loop
         if not Is_Letter (Name_Buffer (Index)) then
            return No_Name;

         else
            loop
               Index := Index + 1;

               exit when Index >= Name_Len;

               if Name_Buffer (Index) = '_' then
                  if Name_Buffer (Index + 1) = '_' then
                     return No_Name;
                  end if;
               end if;

               exit when Name_Buffer (Index) = '-';

               if Name_Buffer (Index) /= '_'
                 and then not Is_Alphanumeric (Name_Buffer (Index))
               then
                  return No_Name;
               end if;

            end loop;
         end if;

         if Index >= Name_Len then
            if Is_Alphanumeric (Name_Buffer (Name_Len)) then

               --  All checks have succeeded. Return name in Name_Buffer

               return Name_Find;

            else
               return No_Name;
            end if;

         elsif Name_Buffer (Index) = '-' then
            Index := Index + 1;
         end if;
      end loop;
   end Project_Name_From;

   --------------------------
   -- Project_Path_Name_Of --
   --------------------------

   function Project_Path_Name_Of
     (Project_File_Name : String;
      Directory         : String) return String
   is
      Result : String_Access;

   begin
      if Current_Verbosity = High then
         Write_Str  ("Project_Path_Name_Of (""");
         Write_Str  (Project_File_Name);
         Write_Str  (""", """);
         Write_Str  (Directory);
         Write_Line (""");");
      end if;

      if not Is_Absolute_Path (Project_File_Name) then
         --  First we try <directory>/<file_name>.<extension>

         if Current_Verbosity = High then
            Write_Str  ("   Trying ");
            Write_Str  (Directory);
            Write_Char (Directory_Separator);
            Write_Str (Project_File_Name);
            Write_Line (Project_File_Extension);
         end if;

         Result :=
           Locate_Regular_File
           (File_Name => Directory & Directory_Separator &
              Project_File_Name & Project_File_Extension,
            Path      => Project_Path.all);

         --  Then we try <directory>/<file_name>

         if Result = null then
            if Current_Verbosity = High then
               Write_Str  ("   Trying ");
               Write_Str  (Directory);
               Write_Char (Directory_Separator);
               Write_Line (Project_File_Name);
            end if;

            Result :=
              Locate_Regular_File
              (File_Name => Directory & Directory_Separator &
                 Project_File_Name,
               Path      => Project_Path.all);
         end if;
      end if;

      if Result = null then

         --  Then we try <file_name>.<extension>

         if Current_Verbosity = High then
            Write_Str  ("   Trying ");
            Write_Str (Project_File_Name);
            Write_Line (Project_File_Extension);
         end if;

         Result :=
           Locate_Regular_File
           (File_Name => Project_File_Name & Project_File_Extension,
            Path      => Project_Path.all);
      end if;

      if Result = null then

         --  Then we try <file_name>

         if Current_Verbosity = High then
            Write_Str  ("   Trying ");
            Write_Line  (Project_File_Name);
         end if;

         Result :=
           Locate_Regular_File
           (File_Name => Project_File_Name,
            Path      => Project_Path.all);
      end if;

      --  If we cannot find the project file, we return an empty string

      if Result = null then
         return "";

      else
         declare
            Final_Result : constant String :=
                             GNAT.OS_Lib.Normalize_Pathname
                               (Result.all,
                                Resolve_Links  => False,
                                Case_Sensitive => True);
         begin
            Free (Result);
            return Final_Result;
         end;
      end if;
   end Project_Path_Name_Of;

begin
   --  Initialize Project_Path during package elaboration

   if Prj_Path.all = "" then
      Project_Path := new String'(".");
   else
      Project_Path := new String'("." & Path_Separator & Prj_Path.all);
   end if;
end Prj.Part;
