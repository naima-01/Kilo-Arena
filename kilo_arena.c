/* Kilo -- A simple editor with custom arena allocator */

#define KILO_VERSION "0.0.2-arena"

#define _POSIX_C_SOURCE 200809L

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include "arena.h"

/* Arena configuration */
#define EDITOR_ARENA_SIZE (32 * 1024 * 1024)  /* 32 MB arena for text buffers */
#define BULK_OP_ARENA_SIZE (4 * 1024 * 1024)  /* 4 MB for temporary operations */

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2
#define HL_MLCOMMENT 3
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

/* Key definitions */
#define TAB 9
#define BACKSPACE 127
#define ESC 27

enum KEY_ACTION {
    KEY_NULL = 0,
    CTRL_C = 3,
    CTRL_D = 4,
    CTRL_F = 6,
    CTRL_H = 8,
    CTRL_L = 12,
    ENTER = 13,
    CTRL_Q = 17,
    CTRL_S = 19,
    CTRL_U = 21,
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

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[2];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_oc;
} erow;

struct editorConfig {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    int rawmode;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    arena_t *arena;
    arena_t *temp_arena;
};

static struct editorConfig E;
static struct termios orig_termios;

/* Function prototypes */
void editorSetStatusMessage(const char *fmt, ...);
void editorUpdateSyntax(erow *row);
void editorUpdateRow(erow *row);
int editorRowHasOpenComment(erow *row);

/* Arena-aware memory management */
void* editor_malloc(size_t size) {
    return arena_alloc(E.arena, size);
}

void editor_free(void *ptr) {
    (void)ptr;
}

/* Terminal handling functions */
void disableRawMode(int fd) {
    if (E.rawmode) {
        tcsetattr(fd, TCSAFLUSH, &orig_termios);
        E.rawmode = 0;
    }
}

void editorAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

int enableRawMode(int fd) {
    struct termios raw;
    
    if (E.rawmode) return 0;
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;
    
    raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) goto fatal;
    E.rawmode = 1;
    return 0;
    
fatal:
    errno = ENOTTY;
    return -1;
}

int editorReadKey(int fd) {
    int nread;
    char c, seq[3];
    while ((nread = read(fd, &c, 1)) == 0);
    if (nread == -1) exit(1);
    
    while (1) {
        switch(c) {
            case ESC:
                if (read(fd, seq, 1) == 0) return ESC;
                if (read(fd, seq + 1, 1) == 0) return ESC;
                
                if (seq[0] == '[') {
                    if (seq[1] >= '0' && seq[1] <= '9') {
                        if (read(fd, seq + 2, 1) == 0) return ESC;
                        if (seq[2] == '~') {
                            switch(seq[1]) {
                                case '3': return DEL_KEY;
                                case '5': return PAGE_UP;
                                case '6': return PAGE_DOWN;
                            }
                        }
                    } else {
                        switch(seq[1]) {
                            case 'A': return ARROW_UP;
                            case 'B': return ARROW_DOWN;
                            case 'C': return ARROW_RIGHT;
                            case 'D': return ARROW_LEFT;
                            case 'H': return HOME_KEY;
                            case 'F': return END_KEY;
                        }
                    }
                } else if (seq[0] == 'O') {
                    switch(seq[1]) {
                        case 'H': return HOME_KEY;
                        case 'F': return END_KEY;
                    }
                }
                break;
            default:
                return c;
        }
    }
}

int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;
    
    while (i < sizeof(buf) - 1) {
        if (read(ifd, buf + i, 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;
    
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        int orig_row, orig_col, retval;
        retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
        if (retval == -1) goto failed;
        
        if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12) goto failed;
        retval = getCursorPosition(ifd, ofd, rows, cols);
        if (retval == -1) goto failed;
        
        char seq[32];
        snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
        if (write(ofd, seq, strlen(seq)) == -1) {}
        return 0;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    
failed:
    return -1;
}

/* Editor row operations */
void editorInsertRow(int at, char *s, size_t len) {
    if (at > E.numrows) return;
    
    size_t new_size = sizeof(erow) * (E.numrows + 1);
    erow *new_rows = arena_alloc(E.arena, new_size);
    if (!new_rows) {
        editorSetStatusMessage("ERROR: Out of arena memory!");
        return;
    }
    
    if (E.row) {
        memcpy(new_rows, E.row, sizeof(erow) * at);
        memcpy(new_rows + at + 1, E.row + at, sizeof(erow) * (E.numrows - at));
    }
    E.row = new_rows;
    
    erow *row = &E.row[at];
    row->size = len;
    row->chars = arena_alloc(E.arena, len + 1);
    if (row->chars) {
        memcpy(row->chars, s, len);
        row->chars[len] = '\0';
    }
    row->hl = NULL;
    row->hl_oc = 0;
    row->render = NULL;
    row->rsize = 0;
    row->idx = at;
    
    editorUpdateRow(row);
    
    for (int j = at + 1; j <= E.numrows; j++) {
        E.row[j].idx++;
    }
    
    E.numrows++;
    E.dirty++;
}

void editorDelRow(int at) {
    if (at >= E.numrows) return;
    
    for (int j = at; j < E.numrows - 1; j++) {
        E.row[j] = E.row[j + 1];
        E.row[j].idx = j;
    }
    
    E.numrows--;
    E.dirty++;
}

void editorUpdateRow(erow *row) {
    int tabs = 0, nonprint = 0, j, idx;
    
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) tabs++;
        else if (!isprint(row->chars[j])) nonprint++;
    }
    
    row->render = arena_alloc(E.arena, row->size + tabs * 8 + nonprint * 9 + 1);
    
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while ((idx + 1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';
    
    row->hl = arena_alloc(E.arena, row->rsize);
    if (row->hl) {
        memset(row->hl, HL_NORMAL, row->rsize);
    }
    
    editorUpdateSyntax(row);
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at > row->size) {
        int padlen = at - row->size;
        size_t new_size = row->size + padlen + 2;
        char *new_chars = arena_alloc(E.arena, new_size);
        if (!new_chars) return;
        
        if (row->chars) memcpy(new_chars, row->chars, row->size);
        memset(new_chars + row->size, ' ', padlen);
        new_chars[row->size + padlen] = c;
        new_chars[row->size + padlen + 1] = '\0';
        row->chars = new_chars;
        row->size += padlen + 1;
    } else {
        size_t new_size = row->size + 2;
        char *new_chars = arena_alloc(E.arena, new_size);
        if (!new_chars) return;
        
        if (row->chars) {
            memcpy(new_chars, row->chars, at);
            memcpy(new_chars + at + 1, row->chars + at, row->size - at + 1);
        }
        new_chars[at] = c;
        row->chars = new_chars;
        row->size++;
    }
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    size_t new_size = row->size + len + 1;
    char *new_chars = arena_alloc(E.arena, new_size);
    if (!new_chars) return;
    
    if (row->chars) memcpy(new_chars, row->chars, row->size);
    memcpy(new_chars + row->size, s, len);
    new_chars[row->size + len] = '\0';
    row->chars = new_chars;
    row->size += len;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (row->size <= at) return;
    
    size_t new_size = row->size;
    char *new_chars = arena_alloc(E.arena, new_size);
    if (!new_chars) return;
    
    if (row->chars) {
        memcpy(new_chars, row->chars, at);
        memcpy(new_chars + at, row->chars + at + 1, row->size - at - 1);
    }
    new_chars[new_size - 1] = '\0';
    row->chars = new_chars;
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* Syntax highlighting */
int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL;
}

int editorRowHasOpenComment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize - 2] != '*' ||
                            row->render[row->rsize - 1] != '/'))) return 1;
    return 0;
}

void editorUpdateSyntax(erow *row) {
    if (!row->hl) return;
    if (E.syntax == NULL) {
        memset(row->hl, HL_NORMAL, row->rsize);
        return;
    }
    
    int i, prev_sep, in_string, in_comment;
    char *p;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;
    
    p = row->render;
    i = 0;
    while (*p && isspace(*p)) { p++; i++; }
    prev_sep = 1;
    in_string = 0;
    in_comment = 0;
    
    if (row->idx > 0 && editorRowHasOpenComment(&E.row[row->idx - 1]))
        in_comment = 1;
    
    while (*p) {
        if (prev_sep && *p == scs[0] && *(p + 1) == scs[1]) {
            memset(row->hl + i, HL_COMMENT, row->size - i);
            return;
        }
        
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p + 1) == mce[1]) {
                row->hl[i + 1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++; i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p + 1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i + 1] = HL_MLCOMMENT;
            p += 2; i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }
        
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i + 1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }
        
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }
        
        if ((isdigit(*p) && (prev_sep || row->hl[i - 1] == HL_NUMBER)) ||
            (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }
        
        if (prev_sep && keywords) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;
                
                if (!memcmp(p, keywords[j], klen) && is_separator(*(p + klen))) {
                    memset(row->hl + i, kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }
        
        prev_sep = is_separator(*p);
        p++; i++;
    }
    
    int oc = editorRowHasOpenComment(row);
    if (row->hl_oc != oc && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
    row->hl_oc = oc;
}

int editorSyntaxToColor(int hl) {
    switch(hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 32;
        case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        default: return 37;
    }
}

void editorSelectSyntaxHighlight(char *filename) {
    static char *C_HL_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
    static char *C_HL_keywords[] = {
        "auto", "break", "case", "continue", "default", "do", "else", "enum",
        "extern", "for", "goto", "if", "register", "return", "sizeof", "static",
        "struct", "switch", "typedef", "union", "volatile", "while", "NULL",
        "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
        "void|", "short|", "const|", NULL
    };
    
    static struct editorSyntax C_syntax = {
        C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    };
    
    if (filename) {
        char *dot = strrchr(filename, '.');
        if (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 ||
                    strcmp(dot, ".cpp") == 0 || strcmp(dot, ".hpp") == 0)) {
            E.syntax = &C_syntax;
            return;
        }
    }
    E.syntax = NULL;
}

/* Editor operations */
void editorInsertChar(int c) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    
    if (!row) {
        while (E.numrows <= filerow)
            editorInsertRow(E.numrows, "", 0);
        row = &E.row[filerow];
    }
    
    editorRowInsertChar(row, filecol, c);
    if (E.cx == E.screencols - 1)
        E.coloff++;
    else
        E.cx++;
    E.dirty++;
}

void editorInsertNewline(void) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    
    if (!row) {
        if (filerow == E.numrows) {
            editorInsertRow(filerow, "", 0);
            goto fixcursor;
        }
        return;
    }
    
    if (filecol >= row->size) filecol = row->size;
    if (filecol == 0) {
        editorInsertRow(filerow, "", 0);
    } else {
        editorInsertRow(filerow + 1, row->chars + filecol, row->size - filecol);
        row = &E.row[filerow];
        if (row->chars) {
            char *new_chars = arena_alloc(E.arena, filecol + 1);
            if (new_chars) {
                memcpy(new_chars, row->chars, filecol);
                new_chars[filecol] = '\0';
                row->chars = new_chars;
                row->size = filecol;
                editorUpdateRow(row);
            }
        }
    }
    
fixcursor:
    if (E.cy == E.screenrows - 1) {
        E.rowoff++;
    } else {
        E.cy++;
    }
    E.cx = 0;
    E.coloff = 0;
}

void editorDelChar(void) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    
    if (!row || (filecol == 0 && filerow == 0)) return;
    
    if (filecol == 0) {
        filecol = E.row[filerow - 1].size;
        editorRowAppendString(&E.row[filerow - 1], row->chars, row->size);
        editorDelRow(filerow);
        if (E.cy == 0)
            E.rowoff--;
        else
            E.cy--;
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols - E.cx) + 1;
            E.cx -= shift;
            E.coloff += shift;
        }
    } else {
        editorRowDelChar(row, filecol - 1);
        if (E.cx == 0 && E.coloff)
            E.coloff--;
        else
            E.cx--;
    }
    if (row) editorUpdateRow(row);
    E.dirty++;
}

void editorMoveCursor(int key) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    
    switch(key) {
        case ARROW_LEFT:
            if (E.cx == 0) {
                if (E.coloff) {
                    E.coloff--;
                } else {
                    if (filerow > 0) {
                        E.cy--;
                        E.cx = E.row[filerow - 1].size;
                        if (E.cx > E.screencols - 1) {
                            E.coloff = E.cx - E.screencols + 1;
                            E.cx = E.screencols - 1;
                        }
                    }
                }
            } else {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (row && filecol < row->size) {
                if (E.cx == E.screencols - 1) {
                    E.coloff++;
                } else {
                    E.cx++;
                }
            } else if (row && filecol == row->size) {
                E.cx = 0;
                E.coloff = 0;
                if (E.cy == E.screenrows - 1) {
                    E.rowoff++;
                } else {
                    E.cy++;
                }
            }
            break;
        case ARROW_UP:
            if (E.cy == 0) {
                if (E.rowoff) E.rowoff--;
            } else {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (filerow < E.numrows) {
                if (E.cy == E.screenrows - 1) {
                    E.rowoff++;
                } else {
                    E.cy++;
                }
            }
            break;
    }
    
    filerow = E.rowoff + E.cy;
    filecol = E.coloff + E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    int rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol - rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

/* File operations */
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    
    *buflen = totlen;
    totlen++;
    
    char *buf = arena_alloc(E.temp_arena, totlen);
    if (!buf) return NULL;
    
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

int editorOpen(char *filename) {
    FILE *fp;
    
    arena_reset(E.arena);
    E.dirty = 0;
    free(E.filename);
    size_t fnlen = strlen(filename) + 1;
    E.filename = malloc(fnlen);
    memcpy(E.filename, filename, fnlen);
    
    fp = fopen(filename, "r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            line[--linelen] = '\0';
        editorInsertRow(E.numrows, line, linelen);
    }
    
    free(line);
    fclose(fp);
    E.dirty = 0;
    
    return 0;
}

int editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    if (!buf) return 1;
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) goto writeerr;
    
    if (ftruncate(fd, len) == -1) goto writeerr;
    if (write(fd, buf, len) != len) goto writeerr;
    
    close(fd);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written on disk", len);
    return 0;
    
writeerr:
    if (fd != -1) close(fd);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
    return 1;
}

/* Screen refresh */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(new + ab->len, s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    
    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff + y;
        
        if (filerow >= E.numrows) {
            abAppend(&ab, "~\x1b[0K\r\n", 7);
            continue;
        }
        
        r = &E.row[filerow];
        int len = r->rsize - E.coloff;
        int current_color = -1;
        
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render + E.coloff;
            unsigned char *hl = r->hl + E.coloff;
            
            for (int j = 0; j < len; j++) {
                if (hl[j] == HL_NONPRINT) {
                    char sym;
                    abAppend(&ab, "\x1b[7m", 4);
                    if (c[j] <= 26)
                        sym = '@' + c[j];
                    else
                        sym = '?';
                    abAppend(&ab, &sym, 1);
                    abAppend(&ab, "\x1b[0m", 4);
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(&ab, c + j, 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        current_color = color;
                        abAppend(&ab, buf, clen);
                    }
                    abAppend(&ab, c + j, 1);
                }
            }
        }
        abAppend(&ab, "\x1b[39m", 5);
        abAppend(&ab, "\x1b[0K", 4);
        abAppend(&ab, "\r\n", 2);
    }
    
    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.rowoff + E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(&ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppend(&ab, " ", 1);
            len++;
        }
    }
    abAppend(&ab, "\x1b[0m\r\n", 6);
    
    abAppend(&ab, "\x1b[0K", 4);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(&ab, E.statusmsg, msglen <= E.screencols ? msglen : E.screencols);
    
    int cx = 1;
    int filerow = E.rowoff + E.cy;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    if (row) {
        for (int j = E.coloff; j < (E.cx + E.coloff); j++) {
            if (j < row->size && row->chars[j] == TAB)
                cx += 7 - ((cx) % 8);
            cx++;
        }
    }
    
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, cx);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* Editor functions */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorFind(int fd) {
    char query[256] = {0};
    int qlen = 0;
    int last_match = -1;
    int find_next = 0;
    int saved_hl_line = -1;
    char *saved_hl = NULL;
    
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;
    
    while (1) {
        editorSetStatusMessage("Search: %s (Use ESC/Arrows/Enter)", query);
        editorRefreshScreen();
        
        int c = editorReadKey(fd);
        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            last_match = -1;
        } else if (c == ESC || c == ENTER) {
            if (c == ESC) {
                E.cx = saved_cx; E.cy = saved_cy;
                E.coloff = saved_coloff; E.rowoff = saved_rowoff;
            }
            if (saved_hl) {
                if (saved_hl_line >= 0 && saved_hl_line < E.numrows && E.row[saved_hl_line].hl) {
                    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
                }
                free(saved_hl);
                saved_hl = NULL;
            }
            editorSetStatusMessage("");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < 255) {
                query[qlen++] = c;
                query[qlen] = '\0';
                last_match = -1;
            }
        }
        
        if (last_match == -1) find_next = 1;
        if (find_next) {
            char *match = NULL;
            int match_offset = 0;
            int i, current = last_match;
            
            for (i = 0; i < E.numrows; i++) {
                current += find_next;
                if (current == -1) current = E.numrows - 1;
                else if (current == E.numrows) current = 0;
                match = strstr(E.row[current].render, query);
                if (match) {
                    match_offset = match - E.row[current].render;
                    break;
                }
            }
            find_next = 0;
            
            if (saved_hl) {
                if (saved_hl_line >= 0 && saved_hl_line < E.numrows && E.row[saved_hl_line].hl) {
                    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
                }
                free(saved_hl);
                saved_hl = NULL;
            }
            
            if (match) {
                erow *row = &E.row[current];
                last_match = current;
                if (row->hl) {
                    saved_hl_line = current;
                    saved_hl = malloc(row->rsize);
                    memcpy(saved_hl, row->hl, row->rsize);
                    memset(row->hl + match_offset, HL_MATCH, qlen);
                }
                E.cy = 0;
                E.cx = match_offset;
                E.rowoff = current;
                E.coloff = 0;
                if (E.cx > E.screencols) {
                    int diff = E.cx - E.screencols;
                    E.cx -= diff;
                    E.coloff += diff;
                }
            }
        }
    }
}

void editorProcessKeypress(int fd) {
    static int quit_times = 3;
    int c = editorReadKey(fd);
    
    switch(c) {
        case ENTER: editorInsertNewline(); break;
        case CTRL_C: break;
        case CTRL_Q:
            if (E.dirty && quit_times) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            exit(0);
            break;
        case CTRL_S: editorSave(); break;
        case CTRL_F: editorFind(fd); break;
        case BACKSPACE: case CTRL_H: case DEL_KEY: editorDelChar(); break;
        case PAGE_UP: case PAGE_DOWN:
            if (c == PAGE_UP && E.cy != 0)
                E.cy = 0;
            else if (c == PAGE_DOWN && E.cy != E.screenrows - 1)
                E.cy = E.screenrows - 1;
            {
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_L: break;
        case ESC: break;
        case CTRL_D: 
        {
            char stats_msg[256];
            size_t overhead = E.arena->stats.total_bytes_used - E.arena->stats.total_bytes_allocated;
            double overhead_percent = E.arena->stats.total_bytes_allocated ? 
                (double)overhead / E.arena->stats.total_bytes_allocated * 100 : 0;
            
            snprintf(stats_msg, sizeof(stats_msg),
                "Arena: %zu/%zu bytes (%.2f%%) | Allocs: %zu | Failed: %zu | Overhead: %zu bytes (%.2f%%) | Resets: %zu",
                E.arena->offset,
                E.arena->size,
                (double)E.arena->offset / E.arena->size * 100,
                E.arena->stats.allocation_count,
                E.arena->stats.failed_allocations,
                overhead,
                overhead_percent,
                E.arena->stats.reset_count);
            
            editorSetStatusMessage("%s", stats_msg);
            
            /* Force a refresh to show the status message */
            editorRefreshScreen();
        }
        break;
        default: editorInsertChar(c); break;
    }
    
    quit_times = 3;
}

void updateWindowSize(void) {
    if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows, &E.screencols) == -1) {
        perror("Unable to query the screen for size");
        exit(1);
    }
    E.screenrows -= 2;
}

void handleSigWinCh(int unused) {
    (void)unused;
    updateWindowSize();
    if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
    if (E.cx > E.screencols) E.cx = E.screencols - 1;
    editorRefreshScreen();
}

void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    
    E.arena = arena_create(EDITOR_ARENA_SIZE);
    E.temp_arena = arena_create(BULK_OP_ARENA_SIZE);
    
    if (!E.arena || !E.temp_arena) {
        fprintf(stderr, "Failed to create arena allocator\n");
        exit(1);
    }
    
    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);
}

void editorShutdown(void) {
    arena_destroy(E.arena);
    arena_destroy(E.temp_arena);
    free(E.filename);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: kilo <filename>\n");
        exit(1);
    }
    
    initEditor();
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    enableRawMode(STDIN_FILENO);
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    
    editorShutdown();
    return 0;
}