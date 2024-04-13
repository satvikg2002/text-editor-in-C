/*** includes ***/

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <termio.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editorConfig{
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);      // prints error description
    exit(1);    // exit with exit status 1 
}

void disableRowMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)  // disable row mode so text is visible in terminal
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");     // get terminal attributes
    atexit(disableRowMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);    // disable break condition, input carriage return(ctrl+M), parity check, 8th bit strip, flow control (ctrl+S, ctrl+Q)
    raw.c_lflag &= ~(OPOST);     // disable output processing to prevent "\n" from becoming "\r\n"
    raw.c_cflag |= (CS8);       // bitmask, sets char size to 8 bits per byte
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);    // disable echo, cannon mode, control-V, ctrl-c (SIGINT) and ctrl-z (SIGSTP) using Bitwise AND and NOT
    raw.c_cc[VMIN] = 0;     // min bytes needed before returning
    raw.c_cc[VTIME] = 1;       // max time to wait before return - 1*(100ms)

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");       // set new attr
}

char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if  (nread == -1 && errno != EAGAIN) die("read");      // EAGAIN flag for cygwin
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];      // read response into buffer
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;     // cursor position report ESCAPE Seq
    
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;      // read till "R"
            i++;
    }
    
    buf[i] = '\0';    // strings end with \0
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;   // get row and column info

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {     // window size from ioctl.h
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)       // move cursor to the opposing edge of the terminal
            return -1;

        return getCursorPosition(rows, cols);
    } 
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** output ***/

void editorDrawRows() {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        write(STDERR_FILENO, "~", 1);
        if (y < E.screenrows - 1) write(STDOUT_FILENO, "\r\n", 2);  
    }
}

void editorRefreshScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    editorDrawRows();
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

void editorProcessKeypress() {
    char c = editorReadKey();
    switch (c)
    {
    case CTRL('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    
    default:
        break;
    }
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        write(STDOUT_FILENO, "\x1b[2J", 4);     // escape seq to clear screen
        write(STDOUT_FILENO, "\x1b[H", 3);      // escape seq to reset cursor
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}