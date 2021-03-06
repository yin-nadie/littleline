/* Copyright 2017 Luis Sanz <luis.sanz@gmail.com> */

#include "littleline.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if (defined(__unix__) || defined(unix))
#include <termios.h>
#include <stdlib.h>
#elif (defined(_WIN32) || defined(WIN32))
#include <conio.h>
#define getchar getch
#endif

#include "buffer.h"
#include "history.h"

struct ll_context {
	/* 0 if not yet initialized */
	int initialized;
#if (defined(__unix__) || defined(unix))
	/* Keep here a copy of the original state of the terminal */
	struct termios buffered;
	/* And build a new state here */
	struct termios unbuffered;
#endif
	/* Key bindings */
	struct ll_fsm bindings;
	/* Last command executed */
	int (*last_command) (void);
	/* All written lines */
	struct ll_history history;
	/* To store executed lines */
        char *history_file;
	/* Index of the line currently being viewed */
	int focus;
	/* Index of the character in line where the cursor currently is */
	int cursor;
	/* Index of the character in the printed line where the cursor currently
	 * is; this may be different from cursor if there are non-printable
	 * characters in the buffer */
	int fmt_cursor;
	/* Number of actual characters currently printed in the line */
	int fmt_len;
	/* Current line */
	const char *current;
	/* Buffer for line editing */
	struct ll_buf buffer;
	/* A buffer to copy text */
	struct ll_buf clipboard;
};

/* There can be only one! */
static struct ll_context cl = { 0 };

struct ll_binding LL_ANSI_KEY_BINDINGS[] = {
	{"\x01", ll_beginning_of_line},	/* C-a */
	{"\x02", ll_backward_char},	/* C-b */
	{"\x03", ll_terminate},	/* C-c */
	{"\x04", ll_end_of_file},	/* C-d */
	{"\x05", ll_end_of_line},	/* C-e */
	{"\x06", ll_forward_char},	/* C-f */
	{"\x08", ll_backward_delete_char},	/* C-h */
	{"\x0A", ll_accept_line},	/* C-j */
	{"\x0B", ll_forward_kill_line},	/* C-k */
	{"\x0E", ll_next_history},	/* C-n */
	{"\x10", ll_previous_history},	/* C-p */
	{"\x15", ll_backward_kill_line},	/* C-u */
	{"\x16", ll_verbatim},	/* C-v */
	{"\x17", ll_backward_kill_word},	/* C-w */
	{"\x19", ll_yank},		/* C-y */
	{"\x1B" "b", ll_backward_word},	/* M-b */
	{"\x1B" "f", ll_forward_word},	/* M-f */

	/* ANSI sequences */
	{"\x1B[A", ll_previous_history},	/* Up */
	{"\x1B[B", ll_next_history},	/* Down */
	{"\x1B[C", ll_forward_char},	/* Right */
	{"\x1B[D", ll_backward_char},	/* Left */
	{"\x1B[3~", ll_delete_char},	/* Delete */
	{"\x1B[7~", ll_beginning_of_line},	/* Home */
	{"\x1B[8~", ll_end_of_line},	/* End */
	{"\x7F", ll_backward_delete_char},	/* Backspace */

	{NULL}
};

/* Setup keyboard */
static int keyboard_init(void);
/* Setdown keyboard */
static void keyboard_deinit(void);
/* Get next character */
static int keyboard_get(void);
/* Reprint the current line */
static void reprint_line(void);
/* Handle a character or sequence of such */
static int handle_character(void);
/* Copy the current line to the buffer */
static int pop_line(void);
/* Push the line currently being edited to the log and create a new one */
static int push_line(void);
/* Insert a string where the cursor is */
static int insert_str(const char *str, size_t len);
/* Insert a character where the cursor is */
static int insert_char(int c);

#if (defined(__unix__) || defined(unix))
static int keyboard_init(void)
{
	/* Disable buffering in stdin. */
	tcgetattr(0, &cl.buffered);
	/* unbuffered is the same as buffered but */
	cl.unbuffered = cl.buffered;
	/* disable "canonical" mode */
	cl.unbuffered.c_lflag &= (~ICANON);
	/* don't echo the character */
	cl.unbuffered.c_lflag &= (~ECHO);
	/* don't automatically handle ^C */
	/*cl.unbuffered.c_lflag &= (~ISIG); */
	/* timeout (tenths of a second) */
	cl.unbuffered.c_cc[VTIME] = 1;
	/* minimum number of characters */
	cl.unbuffered.c_cc[VMIN] = 0;
	tcsetattr(0, TCSANOW, &cl.unbuffered);
	return 0;
}

static void keyboard_deinit(void)
{
	/* Restore stdin settings. */
	tcsetattr(0, TCSANOW, &cl.buffered);
}

static int keyboard_get(void)
{
	int ch;
	do {
		ch = getchar();
	} while (ch == EOF);
	return ch;
}
#endif

static void reprint_line(void)
{
	size_t old_fmt_len;
	const char *it;
	const char *end;
	unsigned char c;
	int i;

	/* We'll need this later */
	old_fmt_len = cl.fmt_len;
	/* Move the cursor to the beginning of the line */
	for (i = 0; i < cl.fmt_cursor; ++i)
		write(STDOUT_FILENO, "\b", 1);
	/* Rebuild the formatted string */
	cl.fmt_cursor = -1;
	cl.fmt_len = 0;
	end = cl.current + strlen(cl.current);
	for (it = cl.current; *it;) {
		if (it - cl.current == cl.cursor)
			cl.fmt_cursor = cl.fmt_len;
		c = *it;
		if (c < 32) {
			/* Handle special characters */
			c += 64;
			write(STDOUT_FILENO, "^", 1);
			write(STDOUT_FILENO, &c, 1);
			cl.fmt_len += 2;
			++it;
		} else if ((c & 0x80) == 0) {
			/* Handle plain ASCII */
			write(STDOUT_FILENO, &c, 1);
			++cl.fmt_len;
			++it;
		} else if ((c & 0xE0) == 0xC0) {
			/* Handle two-byte utf-8 sequence */
			if (end - it < 2)
				break;
			write(STDOUT_FILENO, it, 2);
			++cl.fmt_len;
			it += 2;
		} else if ((c & 0xF0) == 0xE0) {
			/* Handle three-byte utf-8 sequence */
			if (end - it < 3)
				break;
			write(STDOUT_FILENO, it, 3);
			++cl.fmt_len;
			it += 3;
		} else if ((c & 0xF8) == 0xF0) {
			/* Handle four-byte utf-8 sequence */
			if (end - it < 4)
				break;
			write(STDOUT_FILENO, it, 4);
			++cl.fmt_len;
			it += 4;
		} else if ((c & 0xFC) == 0xF8) {
			/* Handle five-byte utf-8 sequence */
			if (end - it < 5)
				break;
			write(STDOUT_FILENO, it, 5);
			++cl.fmt_len;
			it += 5;
		} else {
			/* Handle bad utf-8 sequence */
			write(STDOUT_FILENO, "\\x", 2);
			c = '0' + (*it >> 4);
			write(STDOUT_FILENO, &c, 1);
			c = '0' + (*it & 0xF);
			write(STDOUT_FILENO, &c, 1);
			cl.fmt_len += 4;
			++it;
		}
	}
	/* If the cursor index is still -1, that means it is actually after the end
	 * of the formatted line */
	if (cl.fmt_cursor < 0)
		cl.fmt_cursor = cl.fmt_len;
	/* If the old fmt_len was greater than the current, that means we have
	 * deleted some characters: overwrite them with spaces */
	i = cl.fmt_len;
	while (i < old_fmt_len) {
		write(STDOUT_FILENO, " ", 1);
		++i;
	}
	/* Move back the cursor from the end of the printed line to the actual
	 * cursor position */
	while (i > cl.fmt_cursor) {
		write(STDOUT_FILENO, "\b", 1);
		--i;
	}
}

static int pop_line(void)
{
	if (cl.current != cl.buffer.str) {
		ll_buf_assign(&cl.buffer, cl.current, strlen(cl.current));
		cl.current = cl.buffer.str;
		cl.focus = cl.history.size;
		return 1;
	}
	return 0;
}

static int push_line(void)
{
	ll_history_push(&cl.history, cl.current);
	if (cl.history_file)
		ll_history_write(&cl.history, cl.history_file);
	return 0;
}

static int insert_str(const char *str, size_t len)
{
	pop_line();
	ll_buf_insert(&cl.buffer, cl.cursor, str, len);
	cl.current = cl.buffer.str;
	cl.cursor += len;
	return 0;
}

static int insert_char(int c)
{
	pop_line();
	ll_buf_insert_char(&cl.buffer, cl.cursor, c);
	cl.current = cl.buffer.str;
	++cl.cursor;
	return 0;
}

static int handle_character(void)
{
	char buf[8];
	size_t len = 0;
	int retval;
	int (*func) (void);

	do {
		buf[len] = keyboard_get();
		retval = ll_fsm_feed(&cl.bindings, buf[len], &func);
		++len;
	} while (retval == LL_FSM_INNER_STATE);

	if (retval == LL_FSM_FINAL_STATE) {
		retval = func();
		cl.last_command = func;
		if (retval < 0) {
			putchar(7);
			return 0;
		}
		return retval;
	}
	insert_str(buf, len);
	cl.last_command = NULL;
	return 0;
}

int ll_set_history(size_t max_lines)
{
	ll_history_init(&cl.history, max_lines);
	cl.history_file = NULL;
	return 0;
}

int ll_set_history_with_file(size_t max_lines, const char *path)
{
	ll_history_init(&cl.history, max_lines);
	cl.history_file = strcpy(malloc(strlen(path) + 1), path);
	return ll_history_read(&cl.history, path);
}

int ll_set_key_bindings(const struct ll_binding *bindings)
{
	ll_fsm_init(&cl.bindings, bindings);
	return 0;
}

const char *ll_read(const char *prompt)
{
	int retval;

	if (cl.initialized == 0) {
		cl.initialized = 1;
		cl.last_command = NULL;
		ll_buf_init(&cl.buffer);
		ll_buf_init(&cl.clipboard);
		keyboard_init();
	}

	ll_buf_assign(&cl.buffer, "", 0);
	cl.fmt_len = 0;
	cl.current = cl.buffer.str;
	cl.focus = cl.history.size;
	cl.cursor = 0;
	cl.fmt_cursor = 0;

	write(STDOUT_FILENO, prompt, strlen(prompt));
	write(STDOUT_FILENO, " ", 1);

	do {
		reprint_line();
		retval = handle_character();
	} while (retval == 0);

	reprint_line();
	write(STDOUT_FILENO, "\n", 1);

	if (retval < 0)
		return NULL;
	return cl.buffer.str;
}

int ll_backward_char(void)
{
	if (cl.cursor == 0)
		return -1;
	do
		--cl.cursor;
	while ((cl.current[cl.cursor] & 0xC0) == 0x80);
	return 0;
}

int ll_forward_char(void)
{
	if (cl.current[cl.cursor] == 0)
		return -1;
	do
		++cl.cursor;
	while ((cl.current[cl.cursor] & 0xC0) == 0x80);
	return 0;
}

int ll_backward_word(void)
{
	if (cl.cursor == 0)
		return -1;
	--cl.cursor;
	while (cl.cursor >= 0 && !isalnum(cl.current[cl.cursor]))
		--cl.cursor;
	while (cl.cursor >= 0 && isalnum(cl.current[cl.cursor]))
		--cl.cursor;
	++cl.cursor;
	return 0;
}

int ll_forward_word(void)
{
	if (cl.current[cl.cursor + 1] == 0)
		return -1;
	while (cl.current[cl.cursor] != 0 && !isalnum(cl.current[cl.cursor]))
		++cl.cursor;
	while (cl.current[cl.cursor] != 0 && isalnum(cl.current[cl.cursor]))
		++cl.cursor;
	while (cl.current[cl.cursor] != 0 && !isalnum(cl.current[cl.cursor]))
		++cl.cursor;
	return 0;
}

int ll_beginning_of_line(void)
{
	cl.cursor = 0;
	return 0;
}

int ll_end_of_line(void)
{
	cl.cursor = strlen(cl.current);
	return 0;
}

int ll_previous_history(void)
{
	if (cl.focus == 0)
		return -1;
	--cl.focus;
	cl.current = ll_history_index(&cl.history, cl.focus);
	cl.cursor = strlen(cl.current);
	return 0;
}

int ll_next_history(void)
{
	if (cl.focus == cl.history.size)
		return -1;
	++cl.focus;
	if (cl.focus == cl.history.size)
		cl.current = cl.buffer.str;
	else
		cl.current = ll_history_index(&cl.history, cl.focus);
	cl.cursor = strlen(cl.current);
	return 0;
}

int ll_beginning_of_history(void)
{
	cl.focus = 0;
	cl.current = ll_history_index(&cl.history, cl.focus);
	cl.cursor = strlen(cl.current);
	return 0;
}

int ll_end_of_history(void)
{
	cl.focus = cl.history.size;
	cl.current = cl.buffer.str;
	cl.cursor = strlen(cl.current);
	return 0;
}

int ll_end_of_file(void)
{
	if (strlen(cl.current) == 0)
		return ll_terminate();
	return ll_delete_char();
}

int ll_delete_char(void)
{
	if (cl.current[cl.cursor] == 0)
		return -1;
	pop_line();
	if ((cl.current[cl.cursor] & 0x80) == 0)
		ll_buf_erase(&cl.buffer, cl.cursor, 1);
	else if ((cl.current[cl.cursor] & 0xE0) == 0xC0)
		ll_buf_erase(&cl.buffer, cl.cursor, 2);
	else if ((cl.current[cl.cursor] & 0xF0) == 0xE0)
		ll_buf_erase(&cl.buffer, cl.cursor, 3);
	else if ((cl.current[cl.cursor] & 0xF8) == 0xF0)
		ll_buf_erase(&cl.buffer, cl.cursor, 4);
	else if ((cl.current[cl.cursor] & 0xFC) == 0xF8)
		ll_buf_erase(&cl.buffer, cl.cursor, 5);
	cl.current = cl.buffer.str;
	return 0;
}

int ll_backward_delete_char(void)
{
	if (ll_backward_char() != 0)
		return -1;
	if (ll_delete_char() != 0)
		return -1;
	return 0;
}

int ll_forward_kill_line(void)
{
	size_t len;

	if (cl.current[cl.cursor] == 0)
		return 0;
	pop_line();
	len = cl.buffer.len - cl.cursor;
	if (cl.last_command == ll_forward_kill_word)
		ll_buf_append(&cl.clipboard, cl.buffer.str + cl.cursor, len);
	else
		ll_buf_assign(&cl.clipboard, cl.buffer.str + cl.cursor, len);
	ll_buf_erase(&cl.buffer, cl.cursor, len);
	cl.current = cl.buffer.str;
	return 0;
}

int ll_backward_kill_line(void)
{
	if (cl.cursor == 0)
		return 0;
	pop_line();
	if (cl.last_command == ll_backward_kill_word)
		ll_buf_prepend(&cl.clipboard, cl.buffer.str, cl.cursor);
	else
		ll_buf_assign(&cl.clipboard, cl.buffer.str, cl.cursor);
	ll_buf_erase(&cl.buffer, 0, cl.cursor);
	cl.current = cl.buffer.str;
	cl.cursor = 0;
	return 0;
}

int ll_forward_kill_word(void)
{
	size_t begin;
	size_t len;

	if (cl.current[cl.cursor] == 0)
		return 0;
	pop_line();
	begin = cl.cursor;
	ll_forward_word();
	len = cl.cursor - begin;
	if (cl.last_command == ll_forward_kill_word)
		ll_buf_append(&cl.clipboard, cl.buffer.str + begin, len);
	else
		ll_buf_assign(&cl.clipboard, cl.buffer.str + begin, len);
	ll_buf_erase(&cl.buffer, begin, len);
	cl.current = cl.buffer.str;
	cl.cursor = begin;
	return 0;
}

int ll_backward_kill_word(void)
{
	size_t end;
	size_t len;

	if (cl.cursor == 0)
		return 0;
	pop_line();
	end = cl.cursor;
	ll_backward_word();
	len = end - cl.cursor;
	if (cl.last_command == ll_backward_kill_word)
		ll_buf_prepend(&cl.clipboard, cl.buffer.str + cl.cursor, len);
	else
		ll_buf_assign(&cl.clipboard, cl.buffer.str + cl.cursor, len);
	ll_buf_erase(&cl.buffer, cl.cursor, len);
	cl.current = cl.buffer.str;
	return 0;
}

int ll_yank(void)
{
	if (cl.clipboard.len)
		insert_str(cl.clipboard.str, cl.clipboard.len);
	return 0;
}

int ll_verbatim(void)
{
	reprint_line();
	insert_char(keyboard_get());
	return 0;
}

int ll_accept_line(void)
{
	pop_line();
	push_line();
	return 1;
}

int ll_terminate(void)
{
	write(STDOUT_FILENO, "\n", 1);
	keyboard_deinit();
	exit(EXIT_FAILURE);
}
