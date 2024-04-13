#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <termio.h>

struct termios orig_termios;

void die(const char *s) {
    perror(s);      // prints error description
    exit(1);    // exit with exit status 1 
}

void disableRowMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);  // disable row mode so text is visible in terminal
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);     // get terminal attributes
    atexit(disableRowMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);    // disable break condition, input carriage return(ctrl+M), parity check, 8th bit strip, flow control (ctrl+S, ctrl+Q)
    raw.c_lflag &= ~(OPOST);     // disable output processing to prevent "\n" from becoming "\r\n"
    raw.c_cflag |= (CS8);       // bitmask, sets char size to 8 bits per byte
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);    // disable echo, cannon mode, control-V, ctrl-c (SIGINT) and ctrl-z (SIGSTP) using Bitwise AND and NOT
    raw.c_cc[VMIN] = 0;     // min bytes needed before returning
    raw.c_cc[VTIME] = 1;       // max time to wait before return - 1*(100ms)


    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);       // set new attr
}

int main() {
    enableRawMode();

    while (1) {
        char c = '\0';
        read(STDIN_FILENO, &c, 1);
        
        if (iscntrl(c)) {
            printf("%d\r\n", c);     // print ASCII of control char
        } else {
            printf("%d ('%c')\r\n", c, c);    // print ASCII and char
        }

        if (c == 'q') break;
    }

    return 0;
}