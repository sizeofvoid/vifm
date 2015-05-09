#include <stic.h>

#include <stddef.h> /* NULL size_t */

#include "../../src/column_view.h"

#include "test.h"

static void column_line_print(const void *data, int column_id, const char buf[],
		size_t offset);
static void columns_func(int id, const void *data, size_t buf_len, char *buf);

static const size_t MAX_WIDTH = 80;

static columns_t columns;

static int print_counter;
static int column1_counter;
static int column2_counter;

SETUP()
{
	static column_info_t column_infos[2] = {
		{ .column_id = COL1_ID, .full_width = 100, .text_width = 100,
		  .align = AT_LEFT,     .sizing = ST_AUTO, .cropping = CT_NONE, },
		{ .column_id = COL2_ID, .full_width = 100, .text_width = 100,
		  .align = AT_LEFT,     .sizing = ST_AUTO, .cropping = CT_NONE, },
	};

	print_next = &column_line_print;
	col1_next = &columns_func;
	col2_next = &columns_func;

	print_counter = 0;
	column1_counter = 0;
	column2_counter = 0;

	columns = columns_create();
	columns_add_column(columns, column_infos[0]);
	columns_add_column(columns, column_infos[1]);
}

TEARDOWN()
{
	print_next = NULL;
	col1_next = NULL;
	col2_next = NULL;

	columns_free(columns);
}

static void
column_line_print(const void *data, int column_id, const char buf[],
		size_t offset)
{
	++print_counter;
}

static void
columns_func(int id, const void *data, size_t buf_len, char buf[])
{
	if(id == COL1_ID)
	{
		++column1_counter;
	}
	else
	{
		++column2_counter;
	}
}

TEST(no_columns_one_print_callback_after_creation)
{
	columns_t cols = columns_create();

	columns_format_line(cols, NULL, MAX_WIDTH);

	assert_int_equal(0, column1_counter);
	assert_int_equal(0, column2_counter);
	/* Gap filling callback. */
	assert_int_equal(1, print_counter);

	columns_free(cols);
}

TEST(no_columns_one_print_callback_after_clearing)
{
	columns_clear(columns);

	columns_format_line(columns, NULL, MAX_WIDTH);

	assert_int_equal(0, column1_counter);
	assert_int_equal(0, column2_counter);
	/* Gap filling callback. */
	assert_int_equal(1, print_counter);
}

TEST(number_of_calls_to_format_functions)
{
	columns_format_line(columns, NULL, MAX_WIDTH);

	assert_int_equal(1, column1_counter);
	assert_int_equal(1, column2_counter);
}

TEST(number_of_calls_to_print_function)
{
	columns_format_line(columns, NULL, MAX_WIDTH);

	/* Two more calls are for filling gaps. */
	assert_int_equal(4, print_counter);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
