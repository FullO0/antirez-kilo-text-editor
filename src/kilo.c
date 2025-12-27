/*
 * Building my own text editor from the project Tutorial at 
 * https://viewsourcecode.org/snaptoken/kilo/
 * Author: Christian van der Walt
 * Created on 12/11/2025
 * */

/* TODO: can maby make this more general and use ncurses library to figure out
 * my terminals cababiliteis and adapt the program for the specific terminal we
 * are working in */


/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** function prototypes ***/

void logm(const char *level, const char *func, int line, const char *format, ...);
void editorSetStatusMessage(const char *fmt, ...);
void closeLogFile();

/*** defines ***/

#define KILO_VERSION "0.5.111"
#define LOG_FILE_PATH "/home/christian/kilo.log" /* TODO: make this Dynamic */
#define MAX_MSG_LEN 512

#define KILO_TAB_STOP 8
#define KILO_DIRTY_QUIT_TIMES 3

#define LOG_INFO(...) logm("INFO", __func__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) logm("DEBUG", __func__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) logm("WARN", __func__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) logm("ERROR", __func__, __LINE__, __VA_ARGS__)

#define CTRL_KEY(key) ((key) & 0x1f)

enum editorKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** global variables ***/

int logfd; /* file descriptor for the log file */

/*** data ***/

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s)
{
	/* clear the screen on exit */
	write(STDOUT_FILENO, "\x1b[2J", 4); /* clears screen */
	write(STDOUT_FILENO, "\x1b[H", 3); /* resetes cursor position */

	perror(s);
	exit(1);
}

void disableRawMode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode()
{
	LOG_INFO("Enabling terminal raw mode...");
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
	LOG_INFO("Enabled terminal raw mode.");
}

int editorReadKey()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\xb1';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\xb1';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\xb1';

				/* if escape sequence is page up/down or Home or End key*/
				if (seq[2] == '~') {
					LOG_DEBUG("Read Escape Code: \\x1b %c (%x), %c (%x), %c (%x)",
							  seq[0], seq[0], seq[1], seq[1], seq[2], seq[2]);
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {

				/* If escape sequence is one of the arrow keys or Home/End keys */
				LOG_DEBUG("Read Escape Code: \\x1b %c (%x), %c (%x), %c (%x)",
						  seq[0], seq[0], seq[1], seq[1], seq[2], seq[2]);
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}

		/* Alternative for the Home and End Keys */
		} else if (seq[0] == 'O') {
			LOG_DEBUG("Read Escape Code: \\x1b %c (%x), %c (%x), %c (%x)",
					  seq[0], seq[0], seq[1], seq[1], seq[2], seq[2]);
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\xb1';
	} else {
		LOG_DEBUG("Read Keypress: %c [Hex 0x%02x]", c, (unsigned char) c);
		return c;
	}
}

int getCursorPosition(int *rows, int *cols)
{
	char buf[32];
	unsigned int i;

	/* query the system for the curosr position, then read it from stdin */
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	/* get the cursor report from stdin into the buffer */
	for (i = 0; i < sizeof(buf) - 1; i++) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
	}
	buf[i] = '\0';

	/* parse the buffer to get the rows and cols of the cursor position */
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;
	if (1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {

		/* move the cursor the the bottom right corner by stepping incrementally */
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/
int editorRowCxToRx(erow *row, int cx)

{
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++, rx++)
		if (row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
	return rx;
}

void editorUpdateRow(erow *row)
{
	int j;

	/* count the tabs */
	int tabs = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	/* render chars correctly */
	int idx = 0;
	for (j = 0; j < row->size; j++) {

		/* Tabs */
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';

		/* normal text */
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	LOG_DEBUG("Read line %d from %s as string: \n %s", at, E.filename, s);
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);
	LOG_DEBUG("Rendered line %d from %s as string: \n %s with length %d", 
			  at, E.filename, E.row[at].render, E.row[at].rsize);

	E.numrows++;
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
	LOG_DEBUG("Inserting Character at position %d in row %d", at, E.cy);
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c)
{
	if (E.cy == E.numrows) editorAppendRow("", 0);
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen)
{
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; totlen += E.row[j++].size + 1);
	*buflen = totlen;

	char *buf = malloc(sizeof(char) * totlen);
	char *p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);
	FILE *fp = fopen(filename, "r");
	LOG_INFO("Opening %s for reading", filename);
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
							   line[linelen - 1] == '\r'))
			linelen--;
		editorAppendRow(line, linelen);
	}
	free(line);
	LOG_INFO("Closing %s after reading from it", filename);
	fclose(fp);
	E.dirty = 0;
}

void editorSave()
{
	if (E.filename == NULL) return;

	int len;
	char *buf = editorRowsToString(&len);

	LOG_INFO("Opening file %s to write.", E.filename);
	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {

		LOG_INFO("Truncating file to new length %d...", len);
		if (ftruncate(fd, len) != -1) {

			LOG_INFO("Writing to file...");
			if (write(fd, buf, len) == len) {

				LOG_INFO("%d bytes written to %s successfully.", len, E.filename);
				LOG_INFO("Closing %s.", E.filename);
				close(fd);
				free(buf);
				editorSetStatusMessage("%d bytes written to disk in %s", len, E.filename);
				E.dirty = 0;
				return;
			}
		}

		LOG_INFO("Closing %s.", E.filename);
		close(fd);
	}

	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *new;

	if ((new = realloc(ab->b, ab->len + len)) == NULL) die("realloc");
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab)
{
	free(ab->b);
}

/*** input ***/

void editorMoveCursor(int key)
{
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy > 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows) {
				E.cy++;
			}
			break;
	}

	/* Logic after moving with arrows */
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
	LOG_DEBUG("Moved Cursor to (%d, %d)", E.cx, E.cy);
}

void editorProcessKeypress()
{
	static int quit_times = KILO_DIRTY_QUIT_TIMES;
	int c = editorReadKey();

	switch (c) {
		case '\r':
			/* TODO: */
			break;

		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0) {
				editorSetStatusMessage("WARNING!!! File has unsaved changes... Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4); /* clears screen */
			write(STDOUT_FILENO, "\x1b[H", 3); /* resetes cursor position */

			closeLogFile();
			exit(0);
			break;

		case CTRL_KEY('s'):
			editorSave();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			/* TODO:*/
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}

				int times = E.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
				}
			}
			break;

		case ARROW_LEFT:
		case ARROW_RIGHT:
		case ARROW_UP:
		case ARROW_DOWN:
			editorMoveCursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			/* TODO: */
			break;

		default:
			editorInsertChar(c);
			break;

		quit_times = KILO_DIRTY_QUIT_TIMES;
	}
}

/*** output ***/

void editorScroll()
{
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	/* Vertical Scrolling */
	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}

	/* Horizontal Scrolling */
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab)
{
	int y, welcome_len, padding, filerow;
	char welcome[80];
	LOG_DEBUG("Drawing screen...");
	//LOG_DEBUG("Start drawing screen at rowoff = %d", E.rowoff);
	for (y = 0; y < E.screenrows; y++) {
		filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && y == E.screenrows / 3) {

				/* append the welcome message into a welcome buffer */
				welcome_len = snprintf(welcome, sizeof(welcome),
							"Kilo editor -- version %s", KILO_VERSION);

				/* if there are'nt enough columns to display the welcome message */
				if (welcome_len > E.screencols) welcome_len = E.screencols;

				/* Center the welcome message */
				padding = (E.screencols - welcome_len) / 2;
				if (padding) {
					abAppend(ab, "|", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);

				/* add the welcome message into the main buffer */
				abAppend(ab, welcome, welcome_len);
				//LOG_DEBUG("Drew file row %d with welcome message", filerow);
			} else {
				abAppend(ab, "|", 1);
				//LOG_DEBUG("Drew file row %d with string |", filerow);
			}

		/* Draw non empty rows */
		} else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
			//LOG_DEBUG("Drew file row %d with string %s", filerow, E.row[filerow].render);
		}

		/* erase everything to the right of the cursor */
		abAppend(ab, "\x1b[K", 3);

		/* insert cariage return at every new row */
		abAppend(ab, "\r\n", 2);
	}
	LOG_DEBUG("Drawing screen finished.");
}

void editorDrawStatus(struct abuf *ab)
{
	LOG_INFO("Drawing Statusbar...");
	abAppend(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), " %.20s - %d lines %s", 
					E.filename ? E.filename : "[No Name]", E.numrows,
					E.dirty ? "(Modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d ",
					 E.cy + 1, E.cx + 1);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);

	for (; len < E.screencols; len++) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
		}
	}

	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
	LOG_INFO("Drawing Statusbar finished.");
}

void editorDrawMessageBar(struct abuf *ab)
{
	LOG_INFO("Drawing Messagebar...");
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	LOG_DEBUG("Message contents: %s", E.statusmsg);

	/* Only display msg if it is less than 5 seconds old */
	if (msglen && time(NULL) - E.statusmsg_time < 5)
		abAppend(ab, E.statusmsg, msglen);
	LOG_INFO("Drawing Messagebar finished.");
}

void editorRefreshScreen()
{
	editorScroll();

	struct abuf ab = ABUF_INIT;

	/* Hides the cursor */
	abAppend(&ab, "\x1b[?25l", 6);

	/* Reposition the cursor to the top left part of the screen */
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatus(&ab);
	editorDrawMessageBar(&ab);

	/* moves the cursor to wherever E.cy - E.rowoff (row on the screen) and E.cx - E.coloff (cols on the screen) is */
	char buf[32];
	unsigned int buf_len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
								    (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	if (buf_len >= sizeof(buf)) buf_len = sizeof(buf) - 1;
	abAppend(&ab, buf, buf_len);

	/* unhides the cursor */
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** logging ***/

void initLogFile()
{
	logfd = open(LOG_FILE_PATH,
				O_CREAT | O_WRONLY | O_TRUNC,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (logfd == -1) die("Open Log file error");
	LOG_INFO("Starting kilo version %s Session", KILO_VERSION);
}

void closeLogFile()
{
	if (logfd == -1) return;
	LOG_INFO("Closing kilo %s Session...", KILO_VERSION);
	if (close(logfd) == -1) die("Close Log file error");
}

void logm(const char *level, const char *func, int line, const char *format, ...)
{
	if (logfd == -1) return;

	char buf[MAX_MSG_LEN];
	va_list args;
	const char *color;

	/* Get current time */
	char timebuf[20];
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

	/* Get the color of the header */
	if (strcmp(level, "DEBUG") == 0) color = "\x1b[34m";
	else if (strcmp(level, "INFO") == 0) color = "\x1b[32m";
	else if (strcmp(level, "WARN") == 0) color = "\x1b[33m";
	else if (strcmp(level, "ERROR") == 0) color = "\x1b[31m";

	/* Start log entry with metadata */
	int headlen = snprintf(buf, MAX_MSG_LEN,
						   "%s[%s] [%s] [%s:%d]\x1b[0m ",
						   color, timebuf, level, func, line);

	/* append msg with the extra metadata */
	va_start(args, format);
	int msglen = vsnprintf(buf + headlen, MAX_MSG_LEN - headlen, format, args);
	int len = msglen + headlen;
	va_end(args);

	if (msglen < 0) die("Log Message Format error");

	if (len >= MAX_MSG_LEN) len = MAX_MSG_LEN - 1;

	/* write to log file with a gaurenteed new line character */
	int bytes_written;
	bytes_written = write(logfd, buf, len);
	write(logfd, "\n", 1);
	/* fsync(logfd);  Only Enable if program is crashing*/

	/* Writing to file errors */
	if (bytes_written == -1) die("Write to Log file");
	if (bytes_written < len) die("Write to Log file, Not enough space");
}

/*** init ***/
void initEditor()
{
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2; /* For statusbar and msg */
}

int main(int argc, char *argv[])
{
	initLogFile();
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("Help: Ctrl-S = save | Ctrl-Q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	closeLogFile();
	return EXIT_SUCCESS;
}
