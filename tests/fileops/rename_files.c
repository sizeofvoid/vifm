#include <stic.h>

#include <unistd.h> /* rmdir() symlink() unlink() */

#include "../../src/ui/ui.h"
#include "../../src/filelist.h"
#include "../../src/fops_common.h"
#include "../../src/fops_rename.h"

#include "utils.h"

static void broken_link_name(const char prompt[], const char filename[],
		fo_prompt_cb cb, fo_complete_cmd_func complete, int allow_ee);

SETUP()
{
	view_setup(&lwin);
	set_to_sandbox_path(lwin.curr_dir, sizeof(lwin.curr_dir));

	curr_view = &lwin;
}

TEARDOWN()
{
	view_teardown(&lwin);
}

TEST(generally_renames_files)
{
	char file[] = "file";
	char dir[] = "dir";
	char *names[] = { file, dir };

	create_empty_file(SANDBOX_PATH "/file");
	create_empty_dir(SANDBOX_PATH "/dir");

	populate_dir_list(&lwin, 0);
	lwin.dir_entry[0].marked = 1;
	lwin.dir_entry[1].marked = 1;

	(void)fops_rename(&lwin, names, 2, 0);

	assert_success(rmdir(SANDBOX_PATH "/file"));
	assert_success(unlink(SANDBOX_PATH "/dir"));
}

TEST(renames_files_recursively)
{
	char file1[] = "dir2/file1";
	char file2[] = "dir1/file2";
	char *names[] = { file2, file1 };

	create_empty_dir(SANDBOX_PATH "/dir1");
	create_empty_dir(SANDBOX_PATH "/dir2");
	create_empty_file(SANDBOX_PATH "/dir1/file1");
	create_empty_file(SANDBOX_PATH "/dir2/file2");

	populate_dir_list(&lwin, 0);
	lwin.dir_entry[0].marked = 1;
	lwin.dir_entry[1].marked = 1;

	(void)fops_rename(&lwin, names, 2, 1);

	assert_success(unlink(SANDBOX_PATH "/dir1/file2"));
	assert_success(unlink(SANDBOX_PATH "/dir2/file1"));
	assert_success(rmdir(SANDBOX_PATH "/dir1"));
	assert_success(rmdir(SANDBOX_PATH "/dir2"));
}

TEST(interdependent_rename)
{
	char file2[] = "file2";
	char file3[] = "file3";
	char *names[] = { file2, file3 };

	create_empty_file(SANDBOX_PATH "/file1");
	create_empty_file(SANDBOX_PATH "/file2");

	populate_dir_list(&lwin, 0);
	lwin.dir_entry[0].marked = 1;
	lwin.dir_entry[1].marked = 1;

	(void)fops_rename(&lwin, names, 2, 1);

	/* Make sure reloading doesn't fail with an assert of duplicated file name. */
	populate_dir_list(&lwin, 1);

	assert_success(unlink(SANDBOX_PATH "/file2"));
	assert_success(unlink(SANDBOX_PATH "/file3"));
}

TEST(rename_to_broken_symlink_name, IF(not_windows))
{
	/* symlink() is not available on Windows, but the rest of the code is fine. */
#ifndef _WIN32
	assert_success(symlink("no-such-file", SANDBOX_PATH "/broken-link"));
#endif

	create_empty_file(SANDBOX_PATH "/a-file");

	populate_dir_list(&lwin, 0);
	lwin.list_pos = 0;
	fops_init(&broken_link_name, NULL);
	fops_rename_current(&lwin, 0);

	assert_success(unlink(SANDBOX_PATH "/a-file"));
	assert_success(unlink(SANDBOX_PATH "/broken-link"));
}

static void
broken_link_name(const char prompt[], const char filename[], fo_prompt_cb cb,
		fo_complete_cmd_func complete, int allow_ee)
{
	cb("broken-link");
}

/* No tests for custom/tree view, because control doesn't reach necessary checks
 * when new filenames are provided beforehand (only when user edits them). */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
