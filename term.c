/*** includes ***/
// feature test macros
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define TERM_VERSION "0.0.1"
#define TERM_TAB_STOP 8
#define TERM_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
{
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

enum editorHighlight
{
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

/*** data ***/

typedef struct erow
{
    int size;
    int rsize;
    char *chars;
    char *render;      // rendering tabs
    unsigned char *hl; // syntax highlight
} erow;                // editor row

struct editorConfig
{
    int cx, cy;
    int rx;     // to move cursor acc to tabs
    int rowoff; // row offset from top for vert scroll
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

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s); // prints error description
    exit(1);   // exit with exit status 1
}

void disableRowMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) // disable row mode so text is visible in terminal
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr"); // get terminal attributes
    atexit(disableRowMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // disable break condition, input carriage return(ctrl+M), parity check, 8th bit strip, flow control (ctrl+S, ctrl+Q)
    raw.c_lflag &= ~(OPOST);                                  // disable output processing to prevent "\n" from becoming "\r\n"
    raw.c_cflag |= (CS8);                                     // bitmask, sets char size to 8 bits per byte
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);          // disable echo, cannon mode, control-V, ctrl-c (SIGINT) and ctrl-z (SIGSTP) using Bitwise AND and NOT
    raw.c_cc[VMIN] = 0;                                       // min bytes needed before returning
    raw.c_cc[VTIME] = 1;                                      // max time to wait before return - 1*(100ms)

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr"); // set new attr
}

int editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32]; // read response into buffer
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1; // cursor position report ESCAPE Seq

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break; // read till "R"
        i++;
    }

    buf[i] = '\0'; // strings end with \0
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1; // get row and column info

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {                                                             // window size from ioctl.h
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) // move cursor to the opposing edge of the terminal
            return -1;

        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

void editorUpdateSyntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    int i;
    for (i = 0; i < row->rsize; i++)
        if (isdigit(row->render[i]))
            row->hl[i] = HL_NUMBER;
}

int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
    case HL_NUMBER:
        return 31;
    case HL_MATCH:
        return 34;
    default:
        return 37;
    }
}

/*** row operations ***/

int editorCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            rx += (TERM_TAB_STOP - 1) - (rx % TERM_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
            cur_rx += (TERM_TAB_STOP - 1) - (cur_rx % TERM_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx)
            return cx;
    }

    return cx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j = 0;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (TERM_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % TERM_TAB_STOP != 0)
                row->render[idx++] = ' '; // render tabs
        }
        else
        {
            row->render[idx++] = row->chars[j]; // render other elements
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at); // rewrite all chars in row back by one
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c)
{
    if (E.cy == E.numrows)
        editorInsertRow(E.cy, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    if (E.cx == 0)
        editorInsertRow(E.cy, "", 0);
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx); // new row
        row = &E.row[E.cy];                                             // reassign to prevent memory invalidation
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numrows)
        return;
    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.row[E.cy];

    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

char *editorRowToString(int *buflen)
{
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen; // sum(len of each row of text)

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++)
    {
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
    E.filename = strdup(filename); // creates copy of str

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0; // amount of memory allocated
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // open for read-write, create if not exist, 0644 - text file permission mode
    if (fd != -1)                                      // error handling
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key)
{
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl); // to reset highlight
        saved_hl = NULL;
    } 

    if (key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_DOWN || key == ARROW_RIGHT)
        direction = 1;
    else if (key == ARROW_UP || key == ARROW_LEFT)
        direction = -1;
    else
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1)
        direction = 1;
    int current = last_match;

    int j;
    for (j = 0; j < E.numrows; j++)
    {
        current += direction;
        if (current == -1)
            current = E.numrows - 1;
        else if (current == E.numrows)
            current = 0; // wrap around while search

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match)
        {
            last_match = current; // idx of row we are searching
            E.cy = current;
            E.cx = editorRxToCx(row, match - row->render); // move to start of word
            E.rowoff = E.numrows;                          // scroll to bottom so refresh makes the search result on top
            
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);  // to set highlights
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind()
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query)
        free(query);
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer - to implement dynamic strings for a single write call ***/

struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT \
    {             \
        NULL, 0   \
    }

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len); // alloc memory of len(currStr) + len(nextStr)

    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len); // copy s after end of data in buffer
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b); // free memory
}

/*** output ***/

void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff)
        E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows)
        {
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Term - a Terminal based editor -- version %s", TERM_VERSION);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;                 // truncate in case small terminal
                int padding = (E.screencols - welcomelen) / 2; // centre welcome str
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
                abAppend(ab, "~", 1);
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++)
            {
                if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5); // back to default
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color)
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m]");
        }

        abAppend(ab, "\x1b[K", 3); // erases part of the current line
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4); // invert colors

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows,
                       E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // reset mode
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // set mode

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

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
            callback(buf, c);
    }
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
            E.cx--;
        else if (E.cy > 0) // move to end of prev line
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;

    case ARROW_RIGHT:
        if (row && E.cx < row->size)
            E.cx++;
        else if (row && E.cx == row->size) // move to start of next line
        {
            E.cy++;
            E.cx = 0;
        }
        break;

    case ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;

    case ARROW_DOWN:
        if (E.cy < E.numrows)
            E.cy++;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
        E.cx = rowlen; // confine cursor horizontally
}

void editorProcessKeypress()
{
    static int quit_times = TERM_QUIT_TIMES;

    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
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

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
            E.cy = E.rowoff;
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }
        int times = E.screenrows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = TERM_QUIT_TIMES;
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
    E.statusmsg_time = 0; // timestamp of status message

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1)
    {
        write(STDOUT_FILENO, "\x1b[2J", 4); // escape seq to clear screen
        write(STDOUT_FILENO, "\x1b[H", 3);  // escape seq to reset cursor
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
