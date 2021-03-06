/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef _WIN32
#define _WIN32_WINNT 0x0500
#include <windows.h>
#endif

#include "vifm.h"

#include <curses.h>

#include <unistd.h>

#include <errno.h> /* errno */
#include <locale.h> /* setlocale() LC_ALL */
#include <stddef.h> /* NULL size_t */
#include <stdio.h> /* fprintf() fputs() puts() snprintf() */
#include <stdlib.h> /* EXIT_FAILURE EXIT_SUCCESS exit() srand() system() */
#include <string.h>
#include <time.h> /* time() */

#include "cfg/config.h"
#include "cfg/info.h"
#include "engine/autocmds.h"
#include "compat/fs_limits.h"
#include "engine/cmds.h"
#include "engine/keys.h"
#include "engine/mode.h"
#include "engine/options.h"
#include "engine/variables.h"
#include "int/fuse.h"
#include "int/path_env.h"
#include "int/term_title.h"
#include "int/vim.h"
#include "modes/dialogs/msg_dialog.h"
#include "modes/cmdline.h"
#include "modes/modes.h"
#include "modes/view.h"
#include "ui/cancellation.h"
#include "ui/color_manager.h"
#include "ui/color_scheme.h"
#include "ui/quickview.h"
#include "ui/statusbar.h"
#include "ui/ui.h"
#include "utils/fs.h"
#include "utils/log.h"
#include "utils/macros.h"
#include "utils/path.h"
#include "utils/str.h"
#include "utils/string_array.h"
#include "utils/utils.h"
#include "args.h"
#include "background.h"
#include "bmarks.h"
#include "bracket_notation.h"
#include "builtin_functions.h"
#include "cmd_completion.h"
#include "cmd_core.h"
#include "dir_stack.h"
#include "event_loop.h"
#include "filelist.h"
#include "filetype.h"
#include "flist_pos.h"
#include "fops_common.h"
#include "ipc.h"
#include "marks.h"
#include "ops.h"
#include "opt_handlers.h"
#include "registers.h"
#include "running.h"
#include "signals.h"
#include "status.h"
#include "trash.h"
#include "undo.h"

static int pair_in_use(short int pair);
static void move_pair(short int from, short int to);
static int undo_perform_func(OPS op, void *data, const char src[],
		const char dst[]);
static void parse_received_arguments(char *args[]);
static void remote_cd(FileView *view, const char path[], int handle);
static void check_path_for_file(FileView *view, const char path[], int handle);
static int need_to_switch_active_pane(const char lwin_path[],
		const char rwin_path[]);
static void load_scheme(void);
static void exec_startup_commands(const args_t *args);
static void _gnuc_noreturn vifm_leave(int exit_code, int cquit);

/* Command-line arguments in parsed form. */
static args_t vifm_args;

int
main(int argc, char *argv[])
{
	/* TODO: refactor main() function */

	static const int quit = 0;

	char dir[PATH_MAX];
	char **files = NULL;
	int nfiles = 0;

	if(get_cwd(dir, sizeof(dir)) == NULL)
	{
		perror("getcwd");
		return -1;
	}

	(void)vifm_chdir(dir);
	args_parse(&vifm_args, argc, argv, dir);
	args_process(&vifm_args, 1);

	if(strcmp(vifm_args.lwin_path, "-") == 0 ||
			strcmp(vifm_args.rwin_path, "-") == 0)
	{
		files = read_stream_lines(stdin, &nfiles, 1, NULL, NULL);
		if(reopen_term_stdin() != 0)
		{
			return EXIT_FAILURE;
		}
	}

	(void)setlocale(LC_ALL, "");
	srand(time(NULL));

	cfg_init();

	if(vifm_args.logging)
	{
		init_logger(1, vifm_args.startup_log_path);
	}

	init_filelists();
	regs_init();
	cfg_discover_paths();
	reinit_logger(cfg.log_file);

	/* Commands module also initializes bracket notation and variables. */
	init_commands();

	init_builtin_functions();
	update_path_env(1);

	if(init_status(&cfg) != 0)
	{
		puts("Error during session status initialization.");
		return -1;
	}

	/* Tell file type module what function to use to check availability of
	 * external programs. */
	ft_init(&external_command_exists);
	/* This should be called before loading any configuration file. */
	ft_reset(curr_stats.exec_env_type == EET_EMULATOR_WITH_X);

	init_option_handlers();

	if(!vifm_args.no_configs)
	{
		/* vifminfo must be processed this early so that it can restore last visited
		 * directory. */
		read_info_file(0);
	}

	ipc_init(vifm_args.server_name, &parse_received_arguments);
	/* Export chosen server name to parsing unit. */
	{
		var_val_t value = { .string = (char *)ipc_get_name() };
		var_t var = var_new(VTYPE_STRING, value);
		setvar("v:servername", var);
		var_free(var);
	}

	args_process(&vifm_args, 0);

	bg_init();

	fops_init(&enter_prompt_mode, &prompt_msg_custom);

	set_view_path(&lwin, vifm_args.lwin_path);
	set_view_path(&rwin, vifm_args.rwin_path);

	if(need_to_switch_active_pane(vifm_args.lwin_path, vifm_args.rwin_path))
	{
		swap_view_roles();
	}

	load_initial_directory(&lwin, dir);
	load_initial_directory(&rwin, dir);

	/* Force split view when two paths are specified on command-line. */
	if(vifm_args.lwin_path[0] != '\0' && vifm_args.rwin_path[0] != '\0')
	{
		curr_stats.number_of_windows = 2;
	}

	/* Prepare terminal for further operations. */
	curr_stats.original_stdout = reopen_term_stdout();
	if(curr_stats.original_stdout == NULL)
	{
		return -1;
	}

	if(!setup_ncurses_interface())
	{
		return -1;
	}

	{
		const colmgr_conf_t colmgr_conf = {
			.max_color_pairs = COLOR_PAIRS,
			.max_colors = COLORS,
			.init_pair = &init_pair,
			.pair_content = &pair_content,
			.pair_in_use = &pair_in_use,
			.move_pair = &move_pair,
		};
		colmgr_init(&colmgr_conf);
	}

	init_modes();
	init_undo_list(&undo_perform_func, NULL, &ui_cancellation_requested,
			&cfg.undo_levels);
	load_view_options(curr_view);

	curr_stats.load_stage = 1;

	if(!vifm_args.no_configs)
	{
		load_scheme();
		cfg_load();

		if(strcmp(vifm_args.lwin_path, "-") == 0)
		{
			flist_set(&lwin, "-", dir, files, nfiles);
		}
		else if(strcmp(vifm_args.rwin_path, "-") == 0)
		{
			flist_set(&rwin, "-", dir, files, nfiles);
		}
	}
	/* Load colors in any case to load color pairs. */
	cs_load_pairs();

	cs_write();
	setup_signals();

	/* Ensure trash directories exist, it might not have been called during
	 * configuration file sourcing if there is no `set trashdir=...` command. */
	(void)set_trash_dir(cfg.trash_dir);

	check_path_for_file(&lwin, vifm_args.lwin_path, vifm_args.lwin_handle);
	check_path_for_file(&rwin, vifm_args.rwin_path, vifm_args.rwin_handle);

	curr_stats.load_stage = 2;

	/* Update histories of the views to ensure that their current directories,
	 * which might have been set using command-line parameters, are stored in the
	 * history.  This is not done automatically as history manipulation should be
	 * postponed until views are fully loaded, otherwise there is no correct
	 * information about current file and relative cursor position. */
	save_view_history(&lwin, NULL, NULL, -1);
	save_view_history(&rwin, NULL, NULL, -1);

	/* Trigger auto-commands for initial directories. */
	vle_aucmd_execute("DirEnter", lwin.curr_dir, &lwin);
	vle_aucmd_execute("DirEnter", rwin.curr_dir, &rwin);

	update_screen(UT_FULL);
	modes_update();

	/* Run startup commands after loading file lists into views, so that commands
	 * like +1 work. */
	exec_startup_commands(&vifm_args);

	curr_stats.load_stage = 3;

	event_loop(&quit);

	return 0;
}

/* Checks whether pair is being used at the moment.  Returns non-zero if so and
 * zero otherwise. */
static int
pair_in_use(short int pair)
{
	int i;

	for(i = 0; i < MAXNUM_COLOR; ++i)
	{
		if(cfg.cs.pair[i] == pair || lwin.cs.pair[i] == pair ||
				rwin.cs.pair[i] == pair)
		{
			return 1;
		}
	}

	return 0;
}

/* Substitutes old pair number with the new one. */
static void
move_pair(short int from, short int to)
{
	int i;
	for(i = 0; i < MAXNUM_COLOR; ++i)
	{
		if(cfg.cs.pair[i] == from)
		{
			cfg.cs.pair[i] = to;
		}
		if(lwin.cs.pair[i] == from)
		{
			lwin.cs.pair[i] = to;
		}
		if(rwin.cs.pair[i] == from)
		{
			rwin.cs.pair[i] = to;
		}
	}
}

/* perform_operation() interface adaptor for the undo unit. */
static int
undo_perform_func(OPS op, void *data, const char src[], const char dst[])
{
	return perform_operation(op, NULL, data, src, dst);
}

/* Handles arguments received from remote instance. */
static void
parse_received_arguments(char *argv[])
{
	int argc = 0;
	args_t args = {};

	while(argv[argc] != NULL)
	{
		argc++;
	}

	(void)vifm_chdir(argv[0]);
	opterr = 0;
	args_parse(&args, argc, argv, argv[0]);
	args_process(&args, 0);

	exec_startup_commands(&args);
	args_free(&args);

	if(NONE(vle_mode_is, NORMAL_MODE, VIEW_MODE))
	{
		return;
	}

#ifdef _WIN32
	SwitchToThisWindow(GetConsoleWindow(), TRUE);
	BringWindowToTop(GetConsoleWindow());
	SetForegroundWindow(GetConsoleWindow());
#endif

	if(view_needs_cd(&lwin, args.lwin_path))
	{
		remote_cd(&lwin, args.lwin_path, args.lwin_handle);
	}

	if(view_needs_cd(&rwin, args.rwin_path))
	{
		remote_cd(&rwin, args.rwin_path, args.rwin_handle);
	}

	if(need_to_switch_active_pane(args.lwin_path, args.rwin_path))
	{
		change_window();
	}

	ui_sb_clear();
	curr_stats.save_msg = 0;
}

static void
remote_cd(FileView *view, const char path[], int handle)
{
	char buf[PATH_MAX];

	if(view->explore_mode)
	{
		leave_view_mode();
	}

	if(view == other_view && vle_mode_is(VIEW_MODE))
	{
		leave_view_mode();
	}

	if(curr_stats.view && (handle || view == other_view))
	{
		qv_toggle();
	}

	copy_str(buf, sizeof(buf), path);
	exclude_file_name(buf);

	(void)cd(view, view->curr_dir, buf);
	check_path_for_file(view, path, handle);
}

/* Navigates to/opens (handles) file specified by the path (and file only, no
 * directories). */
static void
check_path_for_file(FileView *view, const char path[], int handle)
{
	if(path[0] == '\0' || is_dir(path) || strcmp(path, "-") == 0)
	{
		return;
	}

	load_dir_list(view, !(cfg.vifm_info&VIFMINFO_SAVEDIRS));
	if(ensure_file_is_selected(view, after_last(path, '/')))
	{
		if(handle)
		{
			open_file(view, FHE_RUN);
		}
	}
}

/* Decides whether active view should be switched based on paths provided for
 * panes on the command-line.  Returns non-zero if so, otherwise zero is
 * returned. */
static int
need_to_switch_active_pane(const char lwin_path[], const char rwin_path[])
{
	/* Forces view switch when path is specified for invisible pane. */
	return lwin_path[0] != '\0'
	    && rwin_path[0] == '\0'
	    && curr_view != &lwin;
}

/* Loads color scheme.  Converts old format to the new one if needed. */
static void
load_scheme(void)
{
	if(cs_have_no_extensions())
	{
		cs_rename_all();
	}

	if(cs_exists(curr_stats.color_scheme))
	{
		cs_load_primary(curr_stats.color_scheme);
	}
}

void
vifm_restart(void)
{
	FileView *tmp_view;

	curr_stats.restart_in_progress = 1;

	/* All user mappings in all modes. */
	vle_keys_user_clear();

	/* User defined commands. */
	execute_cmd("comclear");

	/* Autocommands. */
	vle_aucmd_remove(NULL, NULL);

	/* Directory histories. */
	ui_view_clear_history(&lwin);
	ui_view_clear_history(&rwin);

	/* All kinds of history. */
	(void)hist_reset(&cfg.search_hist, cfg.history_len);
	(void)hist_reset(&cfg.cmd_hist, cfg.history_len);
	(void)hist_reset(&cfg.prompt_hist, cfg.history_len);
	(void)hist_reset(&cfg.filter_hist, cfg.history_len);
	cfg.history_len = 0;

	/* Session status.  Must be reset _before_ options, because options take some
	 * of values from status. */
	(void)reset_status(&cfg);

	/* Options of current pane. */
	reset_options_to_default();
	/* Options of other pane. */
	tmp_view = curr_view;
	curr_view = other_view;
	load_view_options(other_view);
	reset_options_to_default();
	curr_view = tmp_view;

	/* File types and viewers. */
	ft_reset(curr_stats.exec_env_type == EET_EMULATOR_WITH_X);

	/* Undo list. */
	reset_undo_list();

	/* Directory stack. */
	dir_stack_clear();

	/* Registers. */
	regs_reset();

	/* Clear all marks and bookmarks. */
	clear_all_marks();
	bmarks_clear();

	/* Reset variables. */
	clear_envvars();
	init_variables();
	/* This update is needed as clear_variables() will reset $PATH. */
	update_path_env(1);

	reset_views();
	read_info_file(1);
	save_view_history(&lwin, NULL, NULL, -1);
	save_view_history(&rwin, NULL, NULL, -1);

	/* Color schemes. */
	if(stroscmp(curr_stats.color_scheme, DEF_CS_NAME) != 0 &&
			cs_exists(curr_stats.color_scheme))
	{
		if(cs_load_primary(curr_stats.color_scheme) != 0)
		{
			cs_load_defaults();
		}
	}
	else
	{
		cs_load_defaults();
	}
	cs_load_pairs();

	cfg_load();
	exec_startup_commands(&vifm_args);

	curr_stats.restart_in_progress = 0;

	/* Trigger auto-commands for initial directories. */
	vle_aucmd_execute("DirEnter", lwin.curr_dir, &lwin);
	vle_aucmd_execute("DirEnter", rwin.curr_dir, &rwin);

	update_screen(UT_REDRAW);
}

/* Executes list of startup commands. */
static void
exec_startup_commands(const args_t *args)
{
	size_t i;
	for(i = 0; i < args->ncmds; ++i)
	{
		(void)exec_commands(args->cmds[i], curr_view, CIT_COMMAND);
	}
}

void
vifm_try_leave(int write_info, int cquit, int force)
{
	if(!force && bg_has_active_jobs())
	{
		if(!prompt_msg("Warning", "Some of backgrounded commands are still "
					"working.  Quit?"))
		{
			return;
		}
	}

	fuse_unmount_all();

	if(write_info)
	{
		write_info_file();
	}

	if(stats_file_choose_action_set())
	{
		vim_write_empty_file_list();
	}

#ifdef _WIN32
	system("cls");
#endif

	endwin();
	vifm_leave(EXIT_SUCCESS, cquit);
}

void _gnuc_noreturn
vifm_choose_files(const FileView *view, int nfiles, char *files[])
{
	int exit_code;

	/* As curses can do something with terminal on shutting down, disable it
	 * before writing anything to the screen. */
	endwin();

	exit_code = EXIT_SUCCESS;
	if(vim_write_file_list(view, nfiles, files) != 0)
	{
		exit_code = EXIT_FAILURE;
	}
	/* XXX: this ignores nfiles+files. */
	if(vim_run_choose_cmd(view) != 0)
	{
		exit_code = EXIT_FAILURE;
	}

	write_info_file();

	vifm_leave(exit_code, 0);
}

/* Single exit point for leaving vifm, performs only minimum common
 * deinitialization steps. */
static void _gnuc_noreturn
vifm_leave(int exit_code, int cquit)
{
	vim_write_dir(cquit ? "" : flist_get_dir(curr_view));

	if(cquit && exit_code == EXIT_SUCCESS)
	{
		exit_code = EXIT_FAILURE;
	}

	term_title_update(NULL);
	exit(exit_code);
}

void _gnuc_noreturn
vifm_finish(const char message[])
{
	endwin();

	/* Update vifminfo only if we were able to startup, otherwise we can end up
	 * writing from some intermediate half-initialized state.  One particular
	 * case: after vifminfo read, but before configuration is processed, as a
	 * result we write very little information to vifminfo file according to
	 * default value of 'vifminfo' option. */
	if(curr_stats.load_stage == 3)
	{
		write_info_file();
	}

	fprintf(stderr, "%s\n", message);
	LOG_ERROR_MSG("Finishing: %s", message);
	exit(EXIT_FAILURE);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
