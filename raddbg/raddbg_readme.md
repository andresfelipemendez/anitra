# The RAD Debugger (ALPHA)

The RAD Debugger is a native, user-mode, multi-process, graphical debugger. It
currently only supports local-machine Windows x64 debugging with PDBs, but we're
actively working on support and ports for other toolchains and platforms.

## Getting Started

**Launching the debugger with your program information:** To launch the RAD
Debugger with your executable and command line arguments, run `raddbg` from the
command line like so:

```raddbg my_program.exe --foo --bar --baz```

For more information, see the **Command-Line Usage** section.

**Basic commands and keybindings:** Default keyboard shortcuts for common
debugger controls include:

 - **Ctrl + O**: Open Source Code File
 - **F10**: Step Over
 - **F11**: Step Into
 - **Shift + F11**: Step Out
 - **F5**: Run
 - **Ctrl + Shift + X**, or **Pause**: Halt All Processes
 - **Shift + F5**: Kill All Processes
 - **Shift + F6**: Attach To Process
 - **Ctrl + F**: Search For Text (Forwards)
 - **F9**: Toggle Breakpoint At Cursor
 - **Ctrl + Comma**: Focus Next Panel
 - **Ctrl + Shift + Comma**: Focus Previous Panel
 - **Ctrl + Alt + Arrow Key**: Focus Panel In Direction
 - **Ctrl + Tab**: Focus Next Tab
 - **Ctrl + Shift + Tab**: Focus Previous Tab
 - **Ctrl + W**: Close Tab
 - **F1**: Open Palette (lists commands, keybindings, settings, threads,
   processes, modules, types, and many other things)

For more information, see the **Commands** section.

**Configuration files (users and projects):** The RAD Debugger stores
configuration in two files. One is the 'user file', the other is the 'project
file'. Both files are the same format and can store the same kinds of data, but
the user file is preferred by the debugger for more likely user-related data
(windows, keybindings, theme), and the project file is preferred by the debugger
for more likely project-related data (executable debugging targets, breakpoints,
recent source files). Project files are more likely to be what you'd check into
source control, whereas a user file is more likely to have your personal
debugger settings (which will apply identically regardless of which project file
is opened).

The debugger autosaves user and project files. You do not need to manually save
them. To switch which path you are using for either, you can use the `Open User`
(Ctrl + Alt + Shift + O, by default), or `Open Project` (Ctrl + Alt + O, by
default) commands respectively. If a file does not exist at the path you enter
for either, then a new one will be created, and the debugger will begin
autosaving to it. If the initial paths to these files are not specified on the
command line (via `--user` or `--project`), then the debugger uses default paths
for them. The user file path, by default, will be
`%appdata%/raddbg/default.raddbg_user`. The project file path will be whatever
project path was last loaded for the user, or if no such path exists,
`%appdata%/raddbg/default.raddbg_project`. If you suspect that your
configuration files are corrupted or causing the debugger to behave poorly, it
might help to delete your `%appdata%/raddbg` folder (although it'd also help if
you [sent it to us in a bug
report](https://github.com/EpicGamesExt/raddebugger/issues), so that we can
investigate why they were corrupted to begin with!).

For more information, see the `**User & Project Files** section.

**Watch tabs and visualizers:** 'Watch' tabs in the RAD Debugger allow entering
expressions, which can reference variables in your program, and visualize what
their value is when your program is stopped at a particular time. These
expressions roughly follow C expression syntax, but there are a number of
extensions which can be used to visualize expressions in a more useful way. Here
are some examples:

 - `array(pointer, 64)`: Visualizes `pointer` as pointing to a 256-element
   array.
 - `pointer, 64`: Visualizes `pointer` as pointing to a 256-element array.
 - `pointer, count`: Visualizes `pointer` as pointing to a `count`-element
   array.
 - `slice(some_slice_struct)`: Interprets a structure type as containing a base
   pointer and a count (either through an integer, or an 'end pointer'), and
   visualizes the base pointer, pointing to that many elements.
 - `rows(some_struct, a, b, c)`: Displays the value of `some_struct`, but only
   showing members `a`, `b`, and `c`.
 - `omit(some_struct, a, b, c)`: Displays the value of `some_struct`, but only
   showing members other than `a`, `b`, and `c`.
 - `hex(my_int)`: Visualizes the value of `my_int` in base-16 (hexadecimal)
   form.
 - `dec(my_int)`: Visualizes the value of `my_int` in base-10 (decimal) form.
 - `bin(my_int)`: Visualizes the value of `my_int` in base-2 (binary) form.
 - `oct(my_int)`: Visualizes the value of `my_int` in base-8 (octal) form.
 - `my_int, x`: Visualizes the value of `my_int` in base-16 (hexadecimal) form.
 - `my_int, d`: Visualizes the value of `my_int` in base-10 (decimal) form.
 - `my_int, b`: Visualizes the value of `my_int` in base-2 (binary) form.
 - `my_int, o`: Visualizes the value of `my_int` in base-8 (octal) form.
 - `digits(bin(my_int), 32)`: Visualizes the value of `my_int` in base-2
   (binary) form, showing at minimum 32 bits.
 - `my_int.bin().digits(32)`: Visualizes the value of `my_int` in base-2
   (binary) form, showing at minimum 32 bits.
 - `bitmap(base_pointer, width, height, fmt=rgba8)`: Visualizes the data
   starting at `base_pointer` as a bitmap, with width `width` and height
   `height`, with format `rgba8`.

For more information, see the **Views** section.

## Command-Line Usage

When run normally, either by launching through a file explorer or running from a
command line without arguments, `raddbg` will open a new instance of the
debugger, and await further operations. But it also supports a number of command
line options for a number of other purposes. These options are specified with a
`-` or `--` prefix, followed by the name of the option, and if the option
requires an argument value, followed by a `:` or `=`, followed by the argument
value. A list of the possible options follows:

 - `--help` Displays a help menu which documents the possible command line
   options.
 - `--user:<path>` Use to specify the location of a user file which should be
   used. User files are used by default to store user-related settings,
   including window and panel setups, path mapping, and visual settings. If this
   file does not exist, it will be created as necessary. This file will be
   autosaved as user-related changes are made. For more information on user
   files, see the **User & Project Files** section.
 - `--project:<path>` Use to specify the location of a project file which should
   be used. Project files are used by default to store project-related settings.
   If this file does not exist, it will be created as necessary. This file will
   be autosaved as project-related changes are made. For more information on
   project files, read the 'User & Project Files' section.
 - `--auto_step` This will step into all active targets after the debugger
   initially starts.
 - `--auto_run` This will run all active targets after the debugger initially
   starts.
 - `--quit_after_success` (or `-q`) This will close the debugger automatically
   after all processes exit, if they all exited successfully (with code 0), and
   ran with no interruptions.
 - `--ipc` This will launch the debugger in the non-graphical IPC mode, which is
   used to communicate with another running instance of the debugger. The
   debugger instance will launch, send the specified command, then immediately
   terminate. This may be used by editors or other programs to control the
   debugger. For more information on the set of available commands, see the
   **Commands** section. For more information on driving another debugger
   instance with this argument, see the **Driving Another Debugger Instance**
   section.

On the command line, non-options (meaning any command line arguments *not*
prefixed with a `-` or `--`) can also be specified. With normal usage, they are
interpreted as the command line for a target (see the **Targets** section). When
driving another debugger instance (using the `--ipc` argument), this additional
command line text is used to encode a debugger command.

The debugger will stop parsing `-` and `--` prefixes as arguments after seeing a
standalone `--`, *or* after seeing the first non-option argument, when reading
the command line left-to-right. Some examples of command line usage and their
interpretations are below:

 - `raddbg --foo --bar --a:b --c=d test.exe`: All options are used to configure
   `raddbg`. `test.exe` is interpreted as a target executable. `b` is
   interpreted as the parameter for the `a` option. `d` is interpreted as the
   parameter for the `c` option.
 - `raddbg test.exe --foo --bar`: `test.exe`is interpreted as a target
   executable. `--foo --bar` is interpreted as arguments for `test.exe`, and
   thus are *not* used to configure `raddbg`.
 - `raddbg -- test.exe`: `test.exe` is interpreted as a target executable.
 - `raddbg --ipc find_code_location "C:/foo/bar/baz.c:123:1"`: `--ipc`
   configures `raddbg` to drive another instance of `raddbg`. The remainder of
   the text is interpreted as a command.
 - `raddbg "C:/path with spaces/test.exe" --foo --bar`: A target is formed from
   the `test.exe` path, and `--foo --bar` are interpreted as arguments to the
   `test.exe` target.

## Windows, Panels, & Tabs

Each opened debugger window is subdivided into panels. Panels subdivide regions
of their window without overlapping. Each panel can contain multiple tabs, and
can have one tab selected at any time. Tabs can be dragged and dropped between
panels. Each tab is used to view one of the many supported debugger interfaces,
including source code, disassembly, memory, or watch tables. When a tab is
selected, that interface will fill the tab's containing panel's region of the
containing window.

There are no 'special' windows, panels, or tabs; the debugger is written such
that the number of windows, each window's panel organization, and the placement
and arrangement of tabs can all be organized in a large variety of ways.

A list of debugger interfaces, which can occupy tabs, are below:

 - **Watch**: An editable table interface for entering one or many expressions
   to evaluate, and visualizing and exploring their values.
 - **Locals**: Like the Watch tab, but not editable, and displays the set of
   local variables found at the selected thread's current location.
 - **Registers**: Like the Watch tab, but not editable, and displays the set of
   registers for the selected thread.
 - **Globals**: Like the Watch tab, but not editable, and displays all global
   variables from all loaded modules.
 - **Thread Locals**: Like the Watch tab, but not editable, and displays all
   thread-local variables from all loaded modules.
 - **Types**: Like the Watch tab, but not editable, and displays all types from
   all loaded modules.
 - **Procedures**: Like the Watch tab, but not editable, and displays all
   procedures from all loaded modules.
 - **Call Stack**: Displays the currently selected thread's call stack, and
   allows selecting a frame in the call stack, which will unwind the selected
   thread's registers and evaluate expressions within that frame's context.
 - **Targets**: Displays, and allows editing of, the list of all targets.
 - **Breakpoints**: Displays, and allows editing of, the list of all
   breakpoints.
 - **Watch Pins**: Displays, and allows editing of, the list of all watch pins.
 - **Debug Info**: Displays, and allows editing of, the list of all debug info
   files that the debugger has loaded.
 - **Threads**: Displays the list of all threads in all processes to which the
   debugger is attached.
 - **Processes**: Displays the list of all processes to which the debugger is
   attached.
 - **Machines**: Displays the list of all machines to which the debugger is
   connected.
 - **Modules**: Displays the list of all modules in all processes to which the
   debugger is attached.
 - **File Path Map**: Displays, and allows editing of, the list of all file path
   maps. This allows remapping source code paths referenced by debug information
   to other paths on your local machine.
 - **Type Views**: Displays, and allows editing of, the list of all type views,
   which allow automatically adjusting the visualizations for evaluations of a
   certain type.

You can open one of these tabs in any panel by clicking the `+` icon next to
that panel's tabs, or by executing the `Open Tab` command (bound to Ctrl + T) by
default.

## Commands

The debugger, including implicitly with its UI, is operated almost entirely
through 'commands'. Commands may be manually executed in the debugger UI within
the palette (which you can open with F1 by default), or within the commands list
which is opened when you execute the 'Run Command' command. Operations in the
debugger UI are implemented with commands, so if it's ever unclear how to
accomplish some operation through the UI, a useful fallback is searching for and
running the command through the palette.

Commands are also how a debugger instance launched with `--ipc` may communicate
with a primary debugger instance.

A list of commands, how they're referred to textually (for the purposes of
`--ipc` debugger instances), and their descriptions, are below:

 - `Launch and Run` (`launch_and_run`) Starts debugging a new instance of a
   target, then runs.
 - `Launch and Step Into` (`launch_and_step_into`) Starts debugging a new
   instance of a target, then stops at the program's entry point.
 - `Kill` (`kill`) Kills the specified existing attached process(es).
 - `Kill All` (`kill_all`) Kills all attached processes.
 - `Detach` (`detach`) Detaches the specified attached process(es).
 - `Continue` (`continue`) Continues executing all attached processes.
 - `Step Into (Assembly)` (`step_into_inst`) Performs a step that goes into
   calls, at the instruction level.
 - `Step Over (Assembly)` (`step_over_inst`) Performs a step that skips calls,
   at the instruction level.
 - `Step Into (Line)` (`step_into_line`) Performs a step that goes into calls,
   at the source code line level.
 - `Step Over (Line)` (`step_over_line`) Performs a step that skips calls, at
   the source code line level.
 - `Step Out` (`step_out`) Runs to the end of the current function and exits it.
 - `Halt` (`halt`) Halts all attached processes.
 - `Set Thread IP` (`set_thread_ip`) Sets the specified thread's instruction
   pointer at the specified address.
 - `Run To Line` (`run_to_line`) Runs until a particular source line is hit.
 - `Run` (`run`) Runs all targets after starting them if they have not been
   started yet.
 - `Restart` (`restart`) Kills all attached processes, then launches all active
   targets.
 - `Step Into` (`step_into`) Steps once, possibly into function calls, for
   either source lines or instructions (whichever is selected).
 - `Step Over` (`step_over`) Steps once, always over function calls, for either
   source lines or instructions.
 - `Freeze Thread` (`freeze_thread`) Freezes the passed thread.
 - `Thaw Thread` (`thaw_thread`) Thaws the passed thread.
 - `Freeze Process` (`freeze_process`) Freezes the passed process.
 - `Thaw Process` (`thaw_process`) Thaws the passed process.
 - `Freeze Machine` (`freeze_machine`) Freezes the passed machine.
 - `Thaw Machine` (`thaw_machine`) Thaws the passed machine.
 - `Freeze Local Machine` (`freeze_local_machine`) Freezes the local machine.
 - `Thaw Local Machine` (`thaw_local_machine`) Thaws the local machine.
 - `Attach` (`attach`) Attaches to a process that is already running on the
   local machine.
 - `Exit` (`exit`) Exits the debugger.
 - `Open Palette` (`open_palette`) Opens the palette.
 - `Run Command` (`run_command`) Runs a command from the command palette.
 - `Select Thread` (`select_thread`) Selects a thread.
 - `Select Unwind` (`select_unwind`) Selects an unwind frame number for the
   selected thread.
 - `Up One Frame` (`up_one_frame`) Selects the call stack frame above the
   currently selected.
 - `Down One Frame` (`down_one_frame`) Selects the call stack frame below the
   currently selected.
 - `Increase Window Font Size` (`inc_window_font_size`) Increases the window's
   font size by one point.
 - `Decrease Window Font Size` (`dec_window_font_size`) Decreases the window's
   font size by one point.
 - `Increase View Font Size` (`inc_view_font_size`) Increases the view's font
   size by one point.
 - `Decrease View Font Size` (`dec_view_font_size`) Decreases the view's font
   size by one point.
 - `Open New Window` (`open_window`) Opens a new window.
 - `Window Settings` (`window_settings`) Opens settings for a window.
 - `Close Window` (`close_window`) Closes an opened window.
 - `Toggle Fullscreen` (`toggle_fullscreen`) Toggles fullscreen view on the
   active window.
 - `Bring To Front` (`bring_to_front`) Brings all windows to the front, and
   focuses the most recently focused window.
 - `Popup Accept` (`popup_accept`) Accepts the active popup prompt.
 - `Popup Cancel` (`popup_cancel`) Cancels the active popup prompt.
 - `Reset To Default Bindings` (`reset_to_default_bindings`) Resets all
   keybindings to their defaults.
 - `Reset To Default Panel Layout` (`reset_to_default_panels`) Resets the window
   to the default panel layout.
 - `Reset To Compact Panel Layout` (`reset_to_compact_panels`) Resets the window
   to the compact panel layout.
 - `Reset To Simple Panel Layout` (`reset_to_simple_panels`) Resets the window
   to the simple panel layout.
 - `Split Panel Left` (`new_panel_left`) Creates a new panel to the left of the
   active panel.
 - `Split Panel Up` (`new_panel_up`) Creates a new panel at the top of the
   active panel.
 - `Split Panel Right` (`new_panel_right`) Creates a new panel to the right of
   the active panel.
 - `Split Panel Down` (`new_panel_down`) Creates a new panel at the bottom of
   the active panel.
 - `Rotate Panel Columns` (`rotate_panel_columns`) Rotates all panels at the
   closest column level of the panel hierarchy.
 - `Focus Next Panel` (`next_panel`) Cycles the active panel forward.
 - `Focus Previous Panel` (`prev_panel`) Cycles the active panel backwards.
 - `Focus Panel Right` (`focus_panel_right`) Focuses a panel rightward of the
   currently focused panel.
 - `Focus Panel Left` (`focus_panel_left`) Focuses a panel leftward of the
   currently focused panel.
 - `Focus Panel Up` (`focus_panel_up`) Focuses a panel upward of the currently
   focused panel.
 - `Focus Panel Down` (`focus_panel_down`) Focuses a panel downward of the
   currently focused panel.
 - `Close Panel` (`close_panel`) Closes the currently active panel.
 - `Focus Next Tab` (`next_tab`) Focuses the next tab on the active panel.
 - `Focus Previous Tab` (`prev_tab`) Focuses the previous tab on the active
   panel.
 - `Move Tab Right` (`move_tab_right`) Moves the selected tab right one slot.
 - `Move Tab Left` (`move_tab_left`) Moves the selected tab left one slot.
 - `Open New Tab` (`open_tab`) Opens a new tab.
 - `Duplicate Tab` (`duplicate_tab`) Duplicates a tab.
 - `Close Tab` (`close_tab`) Closes the currently opened tab.
 - `Anchor Tab Bar To Top` (`tab_bar_top`) Anchors a panel's tab bar to the top
   of the panel.
 - `Anchor Tab Bar To Bottom` (`tab_bar_bottom`) Anchors a panel's tab bar to
   the bottom of the panel.
 - `Tab Settings` (`tab_settings`) Opens settings for a tab.
 - `Set Current Path` (`set_current_path`) Sets the debugger's current path,
   which is used as a starting point when browsing for files.
 - `Open` (`open`) Opens a file.
 - `Switch To Partner File` (`switch_to_partner_file`) Switches to the focused
   file's partner; or from header to implementation or vice versa.
 - `Show File In Explorer` (`show_file_in_explorer`) Opens the operating
   system's file explorer and shows the selected file.
 - `Go To Disassembly` (`go_to_disassembly`) Goes to the disassembly, if any,
   for a given source code line.
 - `Go To Source` (`go_to_source`) Goes to the source code, if any, for a given
   disassembly line.
 - `New User` (`new_user`) Creates a new user file, and sets the current user
   path as that file's path.
 - `New Project` (`new_project`) Creates a new project file, and sets the
   current project path as that file's path.
 - `Open User` (`open_user`) Opens a user file path, immediately loading it, and
   begins autosaving to it.
 - `Open Project` (`open_project`) Opens a project file path, immediately
   loading it, and begins autosaving to it.
 - `Open Recent Project` (`open_recent_project`) Opens a recently used project
   file.
 - `Save User` (`save_user`) Saves user data to a file, and sets the current
   user path as that path.
 - `Save Project` (`save_project`) Saves project data to a file, and sets the
   current project path as that path.
 - `Write User Data` (`write_user_data`) Writes user data to the active user
   file.
 - `Write Project Data` (`write_project_data`) Writes project data to the active
   project file.
 - `User Settings` (`user_settings`) Opens user settings.
 - `Project Settings` (`project_settings`) Opens project settings.
 - `Edit` (`edit`) Edits the current selection.
 - `Accept` (`accept`) Accepts current changes, or answers prompts in the
   affirmative.
 - `Cancel` (`cancel`) Rejects current changes, exits temporary menus, or
   answers prompts in the negative.
 - `Move Left` (`move_left`) Moves the cursor or selection left.
 - `Move Right` (`move_right`) Moves the cursor or selection right.
 - `Move Up` (`move_up`) Moves the cursor or selection up.
 - `Move Down` (`move_down`) Moves the cursor or selection down.
 - `Move Left Select` (`move_left_select`) Moves the cursor or selection left,
   while selecting.
 - `Move Right Select` (`move_right_select`) Moves the cursor or selection
   right, while selecting.
 - `Move Up Select` (`move_up_select`) Moves the cursor or selection up, while
   selecting.
 - `Move Down Select` (`move_down_select`) Moves the cursor or selection down,
   while selecting.
 - `Move Left Chunk` (`move_left_chunk`) Moves the cursor or selection left one
   chunk.
 - `Move Right Chunk` (`move_right_chunk`) Moves the cursor or selection right
   one chunk.
 - `Move Up Chunk` (`move_up_chunk`) Moves the cursor or selection up one chunk.
 - `Move Down Chunk` (`move_down_chunk`) Moves the cursor or selection down one
   chunk.
 - `Move Up Page` (`move_up_page`) Moves the cursor or selection up one page.
 - `Move Down Page` (`move_down_page`) Moves the cursor or selection down one
   page.
 - `Move Up Whole` (`move_up_whole`) Moves the cursor or selection to the
   beginning of the relevant content.
 - `Move Down Whole` (`move_down_whole`) Moves the cursor or selection to the
   end of the relevant content.
 - `Move Left Chunk Select` (`move_left_chunk_select`) Moves the cursor or
   selection left one chunk.
 - `Move Right Chunk Select` (`move_right_chunk_select`) Moves the cursor or
   selection right one chunk.
 - `Move Up Chunk Select` (`move_up_chunk_select`) Moves the cursor or selection
   up one chunk.
 - `Move Down Chunk Select` (`move_down_chunk_select`) Moves the cursor or
   selection down one chunk.
 - `Move Up Page Select` (`move_up_page_select`) Moves the cursor or selection
   up one page, while selecting.
 - `Move Down Page Select` (`move_down_page_select`) Moves the cursor or
   selection down one page, while selecting.
 - `Move Up Whole Select` (`move_up_whole_select`) Moves the cursor or selection
   to the beginning of the relevant content, while selecting.
 - `Move Down Whole Select` (`move_down_whole_select`) Moves the cursor or
   selection to the end of the relevant content, while selecting.
 - `Move Up Reorder` (`move_up_reorder`) Moves the cursor or selection up, while
   swapping the currently selected element with that upward.
 - `Move Down Reorder` (`move_down_reorder`) Moves the cursor or selection down,
   while swapping the currently selected element with that downward.
 - `Move Home` (`move_home`) Moves the cursor to the beginning of the line.
 - `Move End` (`move_end`) Moves the cursor to the end of the line.
 - `Move Home Select` (`move_home_select`) Moves the cursor to the beginning of
   the line, while selecting.
 - `Move End Select` (`move_end_select`) Moves the cursor to the end of the
   line, while selecting.
 - `Select All` (`select_all`) Selects everything possible.
 - `Delete Single` (`delete_single`) Deletes a single element to the right of
   the cursor, or the active selection.
 - `Delete Chunk` (`delete_chunk`) Deletes a chunk to the right of the cursor,
   or the active selection.
 - `Backspace Single` (`backspace_single`) Deletes a single element to the left
   of the cursor, or the active selection.
 - `Backspace Chunk` (`backspace_chunk`) Deletes a chunk to the left of the
   cursor, or the active selection.
 - `Copy` (`copy`) Copies the active selection to the clipboard.
 - `Cut` (`cut`) Copies the active selection to the clipboard, then deletes it.
 - `Paste` (`paste`) Pastes the current contents of the clipboard.
 - `Insert Text` (`insert_text`) Inserts the text that was used to cause this
   command.
 - `Move Next` (`move_next`) Moves the cursor or selection to the next element.
 - `Move Previous` (`move_prev`) Moves the cursor or selection to the previous
   element.
 - `Go To Line` (`goto_line`) Jumps to a line number in the current code file.
 - `Go To Address` (`goto_address`) Jumps to an address in the current memory or
   disassembly view.
 - `Center Cursor` (`center_cursor`) Snaps the current code view to center the
   cursor.
 - `Contain Cursor` (`contain_cursor`) Snaps the current code view to contain
   the cursor.
 - `Find Next` (`find_next`) Searches the current code file forward (from the
   cursor) for the last searched string.
 - `Find Previous` (`find_prev`) Searches the current code file backwards (from
   the cursor) for the last searched string.
 - `Find Thread` (`find_thread`) Jumps to the passed thread in either source
   code, disassembly, or both if they're already open.
 - `Find Selected Thread` (`find_selected_thread`) Jumps to the selected thread
   in either source code, disassembly, or both if they're already open.
 - `Go To Name` (`goto_name`) Searches for the passed string as a file, a symbol
   in debug info, and more, then jumps to it if possible.
 - `Go To Name At Cursor` (`goto_name_at_cursor`) Searches for the text at the
   cursor as a file, a symbol in debug info, and more, then jumps to it if
   possible.
 - `Toggle Watch Expression` (`toggle_watch_expr`) Adds or removes an expression
   to an opened watch view.
 - `Toggle Watch Expression At Cursor` (`toggle_watch_expr_at_cursor`) Adds or
   removes the expression that the cursor or selection is currently over to an
   opened watch view.
 - `Toggle Watch Expression At Mouse` (`toggle_watch_expr_at_mouse`) Adds or
   removes the expression that the mouse is currently over to an opened watch
   view.
 - `Add Line Breakpoint` (`add_breakpoint`) Places a breakpoint at a given
   location (file path and line number, address, or symbol name).
 - `Toggle Line Breakpoint` (`toggle_breakpoint`) Places or removes a breakpoint
   at a given location (file path and line number, address, or symbol name).
 - `Enable Breakpoint` (`enable_breakpoint`) Enables a breakpoint.
 - `Disable Breakpoint` (`disable_breakpoint`) Disables a breakpoint.
 - `Clear Breakpoints` (`clear_breakpoints`) Removes all breakpoints.
 - `List Breakpoints` (`list_breakpoints`) Lists all breakpoints.
 - `Clear Output` (`clear_output`) Clears all output.
 - `Add Watch Pin` (`add_watch_pin`) Places a watch pin at a given location
   (file path and line number or address).
 - `Load Debug Info` (`load_debug_info`) Loads a debug info file.
 - `Unload Debug Info` (`unload_debug_info`) Unloads a debug info file.
 - `Set Next Statement` (`set_next_statement`) Sets the selected thread's
   instruction pointer to the cursor's position.
 - `Add Target` (`add_target`) Adds a new target.
 - `Select Target` (`select_target`) Selects a target.
 - `Enable Target` (`enable_target`) Enables a target, in addition to all
   targets currently enabled.
 - `Disable Target` (`disable_target`) Disables a target.
 - `Remove Target` (`remove_target`) Removes a target.
 - `Register As Just-In-Time (JIT) Debugger` (`register_as_jit_debugger`)
   Registers the RAD debugger as the just-in-time (JIT) debugger used by the
   operating system.
 - `Find Code Location` (`find_code_location`) Finds a specific source code
   location given file, line, and column coordinates. Opens the file if
   necessary.
 - `Search` (`search`) Begins searching within the active interface.
 - `Search Backwards` (`search_backwards`) Begins searching backwards within the
   active interface.
 - `Open Event Buffer` (`open_event_buffer`) Opens a new event buffer, to which
   debugger events will be written, for external processing.
 - `Close Event Buffer` (`close_event_buffer`) Closes an existing event buffer.
 - `Toggle Developer Menu` (`toggle_dev_menu`) Opens and closes the developer
   menu.
 - `Log Marker` (`log_marker`) Logs a marker in the application log, to denote
   specific points in time within the log.

## Targets

A *target* is one executable and configuration for launching that executable,
including command line arguments and working directory (the directory from which
the executable is launched). Each target may also have a custom label
(prioritized over the executable name when visualizing the target, and also
allows evaluation of the target in a Watch tab), and the name of a custom entry
point function (when the default entry points - `main`, `WinMain`, etc. - are
not desired when stepping into the program upon launch). The debugger can have
several targets at once. Each target can also be enabled or disabled. Some
operations work on all enabled targets - for instance, the `Run` or `Kill All`
commands (standardly bound as F5 or Shift + F5). Enabling and disabling targets
allows one to filter which targets are currently being worked with.

To add a target, you can run the `Add Target` command. A target is also created
automatically from command line arguments - the rules for how this happens can
be found in the `Command-Line Usage` section.

Targets created through command line usage are temporary, meaning they are not
persistently saved across runs of the debugger. To change this, find the target
in the `Targets` tab, and click the `Save To Project` button on that target's
row. After doing so, the target will be restored across runs, and will no longer
need to be specified on the command-line.

## Views

*Views* are used to transform the way that evaluations in the debugger are
visualized. An evaluation is produced by taking an expression string - for
instance, the name of a variable - and using debug info and information from an
attached process' live runtime (memory, registers, and so on) to interpret it.

Evaluations may be visualized in a variety of ways. A 64-bit unsigned integer
may be visualized as a textual representation of the value with a radix of 10. A
32-bit floating-point value may be visualized as a textual representation of the
value. An array of 32-bit floating-point values can be visualized as a list of
textual representations of those values.

But all of these cases may be visualized in a number of other ways, as well. A
64-bit unsigned integer may be more usefully represented with a radix of 16, 8,
or 2. An array of 32-bit floating-point values may encode the R, G, B, and A
components of a color, or vertex positions for 3D geometry, or samples for a
waveform. An array of bytes may encode raw pixel data for an image, or image
data in a compressed format. A struct may have several members which are not
useful to look at all the time. A struct may form the head of a linked list, and
a flat linked list representation may be more preferable than the traditional
watch view representation, which adds an additional layer of hierarchical
nesting with the expansion of each 'next' pointer in a linked list. When
designing the debugger, we felt that the traditional memory view and watch view
representations of data in a debugged-process were not sufficient. Views were
added to the traditional watch table structure to allow per-expression
specification of extra visualization parameters.

Views look just like function calls. They start with the name of the view, a
`(`, then a list of expressions which form the arguments for the view
(optionally delimited by `,`s), followed by a `)`. The meaning of these
arguments can sometimes be inferred through their order; for example, in the
case of `bitmap(ptr, 512, 256)`, the `bitmap` view uses the `ptr` as the primary
expression to interpret as bitmap data, then assumes the widely-used pattern of
width, then height, to interpret the following arguments as the dimensions of
the bitmap. In other cases, arguments must be specifically named. For instance,
the `fmt` argument in `bitmap(ptr, 512, 256, fmt=bgra8)` is required to override
the `bitmap` view's default assumption of RGBA8 bitmap data.

A list of currently-supported views are below:

 - `raw(expr)`: Ignores all views used in `expr`, including those automatically
   applied by type views.
 - `bin(expr)`: Visualizes all numeric values evaluated in `expr` as base-2
   (binary).
 - `oct(expr)`: Visualizes all numeric values evaluated in `expr` as base-8
   (octal).
 - `dec(expr)`: Visualizes all numeric values evaluated in `expr` as base-10
   (decimal).
 - `hex(expr)`: Visualizes all numeric values evaluated in `expr` as base-16
   (hexadecimal).
 - `digits(expr, num)`: Visualizes at least `num` digits in all numeric values
   evaluated in `expr`.
 - `no_string(expr)`: Disables textual string visualization with pointer
   evaluations in `expr`.
 - `no_char(expr)`: Disables character visualization with character or integer
   evaluations in `expr`.
 - `no_addr(expr)`: Disables explicit address visualization with pointer
   evaluations in `expr`.
 - `sequence(expr)`: Interprets `expr` as an integer, encoding how many
   sub-expressions `expr` should expand to produce. This can be used in
   combination with the `table` view to easily generate tables, indexing amongst
   many arrays.
 - `rows(expr, ...)`: Interpreting all post-`expr` arguments as member names,
   only expands to show those members of `expr`.
 - `omit(expr, ...)`: Interpreting all post-`expr` arguments as member names,
   expands to show all members of `expr`, except those with matching names.
 - `range1(expr, min, max)`: Expresses that `expr` is a bounded numeric value
   between `min` and `max`. Interpreted by the debugger to build slider UI for
   an evaluation.
 - `array(expr, count)`: Expresses that `expr` points to `count` values, rather
   than 1, or the fixed size implied by a static array. When expanded, displays
   only that many values.
 - `slice(expr)`: Expresses that `expr` evaluates to a structure type which
   bundles a base pointer and a count (encoding how many elements to which the
   base pointer points). This count can be expressed either as an integer, or as
   an 'end pointer'. When expanded, displays that many elements following that
   base pointer.
 - `table(expr, ...)`: Expresses that `expr` should be expanded normally, but
   interprets all post-`expr` arguments as expressions which should be used to
   form cells for rows which are generated by this expression's expansions. This
   replaces the normal cells which are generated for an expansion in a Watch
   table.
 - `text(expr, [lang = ...])`: Generates a text visualizer, interpreting `expr`
   as (being or pointing to) text.
 - `disasm(expr, [size = ...]`: Generates a disassembly visualizer, interpreting
   `expr` as (being or pointing to) machine code.
 - `memory(expr, [size = ...])`: Generates a memory visualizer, interpreting
   `expr` as (being or pointing to) raw bytes.
 - `bitmap(expr, width, height, [fmt = ...])`: Generates a bitmap visualizer,
   interpreting `expr` as (being or pointing to) raw bitmap data, with `width`
   and `height` as dimensions.
 - `color(expr)`: Generates a color picker, interpreting `expr` as a color
   value.

## Breakpoints

Breakpoints interrupt execution of attached processes. They may be placed on
arbitrary addresses (e.g. by placing a breakpoint on an instruction within a
disassembly view, or with an arbitrary expression, like the name of a function),
or on lines of source code. In the latter case, the source code location is
resolved to code addresses. If there is no code associated with a line of source
code, then the resolution path chooses to use the next closest line of source
code in the same file.

Breakpoints may have stop conditions attached to them. When a breakpoint is hit
by a thread, before it stops execution, the stop condition is evaluated, and if
it evaluates to a nonzero value, only then is execution stopped.

Each breakpoint has a hit count. Every time a breakpoint causes execution to
stop, this counter is increased.

Address breakpoints can also point to data, rather than code. This will cause
execution to stop if some data is read from, written to, or executed. In this
case, the debugger configures the hardware to use the available hardware data
breakpoints feature. To enable this path, in the breakpoint's editor, express
the number of bytes following the address that should be checked for
writes/reads/executions (can be 1, 2, 4, or 8), and select whether or not you
want to break on reads, writes, or executions.

## User & Project Files

Applicable state controlling the debugger's appearance, behavior, targets,
breakpoints, and other configurations is saved and reloaded across runs of the
debugger through both *user files* and *project files*. These files are
auto-saved. These files are written in a textual format which can be hand-edited
as necessary, but they're also continuously re-read and re-written by the
debugger. By default, the debugger uses `%appdata%/raddbg/default.raddbg_user`
for its user file path, and `%appdata%/raddbg/default.raddbg_project` for its
project file path. These paths can be overridden on the command line (see the
'Command-Line Usage' section).

The *user file* defaultly stores file path maps, windows (including their
preferred monitor, placement, and size), each window's panel layout and tabs,
keybindings, theme colors, and fonts.

The *project file* defaultly stores targets, breakpoints, watch pins, and
exception code filters.

Because both can be hand-edited, however, if you want to store something
normally stored in a user file in a project file, or vice versa, this can be
done by hand transferring the textual data from one file to another. There is no
path in the debugger's UI to support this transfer, currently, although this is
planned.

## Driving Another Debugger Instance

When the debugger is launched with the `--ipc` command-line argument, it does
not launch another instance of the graphical debugger. Instead, it launches,
sends a string encoding a command to a running instance of the graphical
debugger, and then terminates. The set of commands which can be sent are
identical to those which can be run from the debugger's UI itself, but these
commands must be encoded textually (through the other command-line arguments).
These commands are described in the 'Commands' section.

