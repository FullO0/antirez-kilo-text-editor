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
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(key) ((key) & 0x1f)

/*** data ***/

struct editorConfig {
	int screenrows;
	int screencols;
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
	return c;
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

/*** input ***/

void editorProcessKeypress()
{
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); /* clears screen */
			write(STDOUT_FILENO, "\x1b[H", 3); /* resetes cursor position */
			exit(0);
			break;
	}
}

/*** output ***/

void editorDrawRows()
{
	int y;
	for (y = 0; y < E.screenrows; y++) {

		/* draws | on the left side of the terminal */
		if (y != E.screenrows - 1) {
			write(STDOUT_FILENO, "|\r\n", 3);
		} else {
			write(STDOUT_FILENO, "|", 1);
		}
	}
}

void editorRefreshScreen()
{
	/* clear the entire screen of the terminal */
	write(STDOUT_FILENO, "\x1b[2J", 4);

	/* Reposition the cursor to the top left part of the screen */
	write(STDOUT_FILENO, "\x1b[H", 3);

	editorDrawRows();

	/* after drawing the rows resets cursor position*/
	write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** init ***/

void initEditor()
{
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main()
{
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return EXIT_SUCCESS;
}
