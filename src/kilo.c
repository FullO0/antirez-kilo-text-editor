/*
 * Building my own text editor from the project Tutorial at 
 * https://viewsourcecode.org/snaptoken/kilo/
 * Author: Christian van der Walt
 * Created on 12/11/2025
 * */

/* Last Finished Chapter: 2 */

/* TODO: can maby make this more general and use ncurses library to figure out
 * my terminals cababiliteis and adapt the program for the specific terminal we
 * are working in */


/*** includes ***/

#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.46"
#define LOG_FILE_PATH "/home/christian/kilo.log" /* TODO: make this Dynamic */
#define MAX_MSG_LEN 512

#define CTRL_KEY(key) ((key) & 0x1f)

/*** global variables ***/

int logfd; /* file descriptor for the log file */

/*** data ***/

struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

/*** function prototypes ***/

void logm(const char *format, ...);
void closeLogFile();

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
}

char editorReadKey()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\xb1') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\xb1';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\xb1';

		if (seq[0] == '[') {

			/* If escape sequence is one of the arrow keys */
			switch (seq[1]) {
				case 'A': return 'w';
				case 'B': return 's';
				case 'C': return 'd';
				case 'D': return 'a';
			}
		}

		return '\xb1';
	} else {
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

void editorMoveCursor(char key)
{
	switch (key) {
		case 'a':
			E.cx--;
			break;
		case 's':
			E.cy++;
			break;
		case 'd':
			E.cx++;
			break;
		case 'w':
			E.cy--;
			break;
	}
}

void editorProcessKeypress()
{
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); /* clears screen */
			write(STDOUT_FILENO, "\x1b[H", 3); /* resetes cursor position */

			closeLogFile();
			exit(0);
			break;
		
		case 'a':
		case 'd':
		case 'w':
		case 's':
			editorMoveCursor(c);
			break;
	}
}

/*** output ***/

void editorDrawRows(struct abuf *ab)
{
	int y, welcome_len, padding;
	char welcome[80];
	for (y = 0; y < E.screenrows; y++) {
		if (y == E.screenrows / 3) {

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
		} else {
			abAppend(ab, "|", 1);
		}

		/* erase everything to the right of the cursor */
		abAppend(ab, "\x1b[K", 3);

		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen()
{
	struct abuf ab = ABUF_INIT;

	/* Hides the cursor */
	abAppend(&ab, "\x1b[?25l", 6);

	/* Reposition the cursor to the top left part of the screen */
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	/* moves the cursor to wherever E.cy (rows) and E.cx (cols) is */
	char buf[32];
	unsigned int buf_len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	if (buf_len >= sizeof(buf)) buf_len = sizeof(buf) - 1;
	abAppend(&ab, buf, buf_len);

	/* unhides the cursor */
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}
/*** logging ***/
void initLogFile()
{
	logfd = open(LOG_FILE_PATH,
				O_CREAT | O_WRONLY | O_APPEND,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (logfd == -1) die("Open Log file");
	logm("--- Opening Kilo %s Session ---\n", KILO_VERSION);
}

void logm(const char *format, ...)
{
	if (logfd == -1) return;

	char buf[MAX_MSG_LEN];
	va_list args;
	int len;

	/* append msg together with variable arguments */
	va_start(args, format);
	len = vsnprintf(buf, MAX_MSG_LEN, format, args);
	va_end(args);

	/* appending errors */
	if (len < 0) die("Logging write error");
	if (len >= MAX_MSG_LEN) len = MAX_MSG_LEN - 1;

	int bytes_written;
	bytes_written = write(logfd, buf, len);

	/* Writing to file errors */
	if (bytes_written == -1) die("Write to Log file");
	if (bytes_written < len) die("Write to Log file, Not enough space");
}

void closeLogFile()
{
	if (logfd == -1) return;
	logm("--- Closing Kilo %s Session ---\n", KILO_VERSION);
	if (close(logfd) == -1) die("Close Log file");
}

/*** init ***/
void initEditor()
{
	E.cx = 0;
	E.cy = 0;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main()
{
	initLogFile();
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	closeLogFile();
	return EXIT_SUCCESS;
}
