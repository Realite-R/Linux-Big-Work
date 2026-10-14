#include "client_input.h"
#include "protocol.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

/* Text offsets are UTF-8 byte offsets; display coordinates are terminal cells. */
typedef struct { int row, column; } position_t;
typedef struct client_editor {
    // 存东西
    char *text;
    size_t capacity, length, cursor;
    // 存每一个东西在排版上放在哪里
    position_t *positions;
    int columns, rows, top, drawn_rows, screen_row, goal_column;
    int pending_byte, pasting, paste_cr, paste_overflow;
    const char *notice;
} editor_t;

static struct termios original_termios;
static int terminal_active, cleanup_registered;
static volatile sig_atomic_t interrupted, resized;

static void restore_terminal(void) {
    if (terminal_active) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
        fputs("\033[?2004l", stdout);
        fflush(stdout);
        terminal_active = 0;
    }
}

/* Keep signal handlers async-signal-safe; cleanup runs in the input loop. */
static void input_signal(int signal_number) {
    if (signal_number == SIGWINCH) resized = 1;
    else interrupted = signal_number;
}

static size_t previous_char(const editor_t *e, size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && ((unsigned char)e->text[at] & 0xc0) == 0x80) --at;
    return at;
}

static size_t next_char(const editor_t *e, size_t at) {
    if (at == e->length) return at;
    ++at;
    while (at < e->length && ((unsigned char)e->text[at] & 0xc0) == 0x80) ++at;
    return at;
}

static int character_width(const editor_t *e, size_t at, int column) {
    if (e->text[at] == '\t') return 4 - column % 4;
    wchar_t value;
    mbstate_t state = {0};
    size_t n = mbrtowc(&value, e->text + at, next_char(e, at) - at, &state);
    if (n == (size_t)-1 || n == (size_t)-2) return 1;
    int width = wcwidth(value);
    return width < 0 ? 1 : width;
}

static void dimensions(editor_t *e) {
    struct winsize size;
    e->columns = 80;
    e->rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_col) e->columns = size.ws_col;
        if (size.ws_row) e->rows = size.ws_row;
    }
}

static int usable_columns(const editor_t *e) {
    /* Explicit wraps avoid the terminal's pending-autowrap cursor state. */
    return e->columns > 1 ? e->columns - 1 : 1;
}

static int prompt_width(const editor_t *e) {
    int width = usable_columns(e);
    return width > 5 ? 5 : width - 1;
}

static void layout(editor_t *e) {
    int width = usable_columns(e), row = 0, column = prompt_width(e);
    for (size_t at = 0; at < e->length; at = next_char(e, at)) {
        if (e->text[at] == '\n') {
            e->positions[at] = (position_t){row, column};
            ++row;
            column = 0;
        } else {
            int advance = character_width(e, at, column);
            if (advance > width) advance = width;
            if (column + advance > width) { ++row; column = 0; }
            /* Tabs are rendered as spaces and can change width after wrapping. */
            if (e->text[at] == '\t') {
                advance = character_width(e, at, column);
                if (advance > width) advance = width;
            }
            e->positions[at] = (position_t){row, column};
            column += advance;
            if (column >= width) { ++row; column = 0; }
        }
    }
    e->positions[e->length] = (position_t){row, column};
}

static void move_up(int rows) {
    if (rows > 0) printf("\033[%dA", rows);
}

static void render_line(const editor_t *e, int row) {
    fputs("\033[36m", stdout);
    if (row == 0) fwrite("you> ", 1, (size_t)prompt_width(e), stdout);
    for (size_t at = 0; at < e->length; at = next_char(e, at)) {
        if (e->positions[at].row != row || e->text[at] == '\n') continue;
        int width = character_width(e, at, e->positions[at].column);
        if (e->text[at] == '\t') {
            if (width > usable_columns(e)) width = usable_columns(e);
            for (int i = 0; i < width; ++i) fputc(' ', stdout);
        } else if (width > usable_columns(e)) fputc('?', stdout);
        else fwrite(e->text + at, 1, next_char(e, at) - at, stdout);
    }
    fputs("\033[0m", stdout);
}

static void redraw(editor_t *e) {
    layout(e);
    int max_rows = e->rows > 1 ? e->rows - 1 : 1;
    int cursor_row = e->positions[e->cursor].row;
    int last_row = e->positions[e->length].row;
    if (e->top > cursor_row) e->top = cursor_row;
    if (cursor_row >= e->top + max_rows) e->top = cursor_row - max_rows + 1;
    if (e->top > last_row - max_rows + 1) {
        e->top = last_row - max_rows + 1;
        if (e->top < 0) e->top = 0;
    }
    int visible = last_row - e->top + 1;
    if (visible > max_rows) visible = max_rows;
    fputs("\r", stdout);
    move_up(e->screen_row);
    fputs("\033[J", stdout);
    for (int i = 0; i < visible; ++i) {
        if (i) fputs("\r\n", stdout);
        render_line(e, e->top + i);
    }
    int rendered_rows = visible;
    if (e->notice != NULL && e->rows > 1) {
        fputs("\r\n", stdout);
        fwrite(e->notice, 1, strnlen(e->notice, (size_t)usable_columns(e)), stdout);
        ++rendered_rows;
    }
    e->screen_row = cursor_row - e->top;
    move_up(rendered_rows - 1 - e->screen_row);
    fputs("\r", stdout);
    if (e->positions[e->cursor].column)
        printf("\033[%dC", e->positions[e->cursor].column);
    e->drawn_rows = rendered_rows;
    fflush(stdout);
}

/* Finish below the editor even when the cursor was in its middle. */
static void finish_display(editor_t *e) {
    int below = e->drawn_rows - 1 - e->screen_row;
    if (below > 0) printf("\033[%dB", below);
    fputs("\r\n", stdout);
    fflush(stdout);
}

static void warn_limit(editor_t *e) {
    e->notice = "Input limit reached; delete text to continue.";
    if (e->pasting) e->paste_overflow = 1;
    else fputc('\a', stdout);
}

static void insert(editor_t *e, const char *bytes, size_t count) {
    if (e->length + count >= e->capacity) { warn_limit(e); return; }
    memmove(e->text + e->cursor + count, e->text + e->cursor,
            e->length - e->cursor + 1);
    memcpy(e->text + e->cursor, bytes, count);
    e->length += count;
    e->cursor += count;
    e->goal_column = -1;
    if (!e->paste_overflow) e->notice = NULL;
}

static void remove_range(editor_t *e, size_t start, size_t end) {
    memmove(e->text + start, e->text + end, e->length - end + 1);
    e->length -= end - start;
    e->cursor = start;
    e->goal_column = -1;
    e->notice = NULL;
    e->paste_overflow = 0;
}

static int frame_fits(const char *text) {
    char escaped[PROTOCOL_MAX_LINE];
    return protocol_escape(text, escaped, sizeof(escaped)) == 0 &&
           strlen(escaped) + 4 <= PROTOCOL_MAX_LINE;
}

static void vertical_move(editor_t *e, int direction) {
    layout(e);
    position_t current = e->positions[e->cursor];
    int row = current.row + direction;
    if (row < 0 || row > e->positions[e->length].row) return;
    if (e->goal_column < 0) e->goal_column = current.column;
    size_t best = e->cursor;
    int distance = -1;
    for (size_t at = 0;; at = next_char(e, at)) {
        if (e->positions[at].row == row) {
            int delta = abs(e->positions[at].column - e->goal_column);
            if (distance < 0 || delta < distance) { best = at; distance = delta; }
        }
        if (at == e->length) break;
    }
    e->cursor = best;
}

static void row_edge(editor_t *e, int end) {
    layout(e);
    int row = e->positions[e->cursor].row;
    for (size_t at = 0;; at = next_char(e, at)) {
        if (e->positions[at].row == row) {
            e->cursor = at;
            if (!end) break;
        }
        if (at == e->length) break;
    }
    e->goal_column = -1;
}

/* -2 means timeout/resize; EOF and errors propagate to the outer loop. */
static int read_byte(editor_t *e, unsigned char *byte, int timeout) {
    if (interrupted) return 0;
    if (e->pending_byte >= 0) {
        *byte = (unsigned char)e->pending_byte;
        e->pending_byte = -1;
        return 1;
    }
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int ready = poll(&pfd, 1, timeout);
    if (ready == 0) return -2;
    if (ready < 0) return errno == EINTR ? -2 : -1;
    ssize_t result = read(STDIN_FILENO, byte, 1);
    if (result < 0 && errno == EINTR) return -2;
    return result < 0 ? -1 : (int)result;
}

enum { KEY_NONE, KEY_ENTER, KEY_PAGE_UP, KEY_PAGE_DOWN };

static int escape_key(editor_t *e) {
    unsigned char byte;
    if (read_byte(e, &byte, 100) != 1) return KEY_NONE;
    if (byte != '[' && byte != 'O') {
        /* Discard OSC/DCS strings too, rather than displaying their contents. */
        if (byte == ']' || byte == 'P') {
            int escape = 0;
            for (int i = 0; i < 4096; ++i) {
                if (read_byte(e, &byte, 100) != 1) break;
                if (byte == 7 || (escape && byte == '\\')) break;
                escape = byte == 27;
            }
        }
        return KEY_NONE;
    }
    char sequence[64];
    size_t used = 0;
    int overflow = 0, complete = 0;
    /* Consume complete unknown sequences even when they exceed our buffer. */
    for (int i = 0; i < 4096; ++i) {
        if (read_byte(e, &byte, 100) != 1) break;
        if (byte < 0x20 || byte > 0x7e) {
            e->pending_byte = byte;
            break;
        }
        if (used + 1 < sizeof(sequence)) sequence[used++] = (char)byte;
        else overflow = 1;
        if (byte >= 0x40 && byte <= 0x7e) { complete = 1; break; }
    }
    sequence[used] = '\0';
    if (!complete || overflow) return KEY_NONE;
    if (strcmp(sequence, "201~") == 0) {
        e->pasting = 0;
        e->paste_cr = 0;
        if (e->paste_overflow) e->notice = "Paste truncated at input limit; edit before sending.";
        return KEY_NONE;
    }
    if (e->pasting) return KEY_NONE;
    if (strcmp(sequence, "5~") == 0) return KEY_PAGE_UP;
    if (strcmp(sequence, "6~") == 0) return KEY_PAGE_DOWN;
    if (strcmp(sequence, "200~") == 0) {
        e->pasting = 1;
        e->paste_cr = e->paste_overflow = 0;
    } else if (strcmp(sequence, "D") == 0) {
        e->cursor = previous_char(e, e->cursor); e->goal_column = -1;
    } else if (strcmp(sequence, "C") == 0) {
        e->cursor = next_char(e, e->cursor); e->goal_column = -1;
    } else if (strcmp(sequence, "A") == 0) vertical_move(e, -1);
    else if (strcmp(sequence, "B") == 0) vertical_move(e, 1);
    else if (strcmp(sequence, "H") == 0 || strcmp(sequence, "1~") == 0 ||
             strcmp(sequence, "7~") == 0) row_edge(e, 0);
    else if (strcmp(sequence, "F") == 0 || strcmp(sequence, "4~") == 0 ||
             strcmp(sequence, "8~") == 0) row_edge(e, 1);
    else if (strcmp(sequence, "3~") == 0) remove_range(e, e->cursor, next_char(e, e->cursor));
    else if (strcmp(sequence, "13;2u") == 0 || strcmp(sequence, "27;2;13~") == 0 ||
             strcmp(sequence, "13;2~") == 0) insert(e, "\n", 1);
    else if (strcmp(sequence, "13u") == 0 || strcmp(sequence, "13;1u") == 0)
        return KEY_ENTER;
    return KEY_NONE;
}

/* Insert only a complete valid character, including during fragmented reads. */
static void insert_character(editor_t *e, unsigned char first) {
    char bytes[4];
    bytes[0] = (char)first;
    int count = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!count) { fputc('\a', stdout); return; }
    for (int i = 1; i < count; ++i) {
        unsigned char byte;
        if (read_byte(e, &byte, 100) != 1) return;
        if ((byte & 0xc0) != 0x80) { e->pending_byte = byte; return; }
        bytes[i] = (char)byte;
    }
    wchar_t value;
    mbstate_t state = {0};
    if (mbrtowc(&value, bytes, (size_t)count, &state) == (size_t)-1) {
        fputc('\a', stdout); return;
    }
    if (count > 1 && wcwidth(value) < 0) return;
    insert(e, bytes, (size_t)count);
}

/* Shared editing semantics for the plain reader and the full-screen loop. */
static int feed_key(editor_t *e, unsigned char byte) {
    if (byte == 3) return CLIENT_EDITOR_EXIT;
    if (byte == 4 && !e->pasting) {
        if (e->length == 0) return CLIENT_EDITOR_EXIT;
        remove_range(e, e->cursor, next_char(e, e->cursor));
    } else if (byte == 27) {
        int key = escape_key(e);
        if (key == KEY_PAGE_UP) return CLIENT_EDITOR_PAGE_UP;
        if (key == KEY_PAGE_DOWN) return CLIENT_EDITOR_PAGE_DOWN;
        if (key == KEY_ENTER) byte = '\r';
        else return CLIENT_EDITOR_EDIT;
    } else if (e->pasting) {
        if (byte == '\r') { insert(e, "\n", 1); e->paste_cr = 1; }
        else if (byte == '\n') {
            if (!e->paste_cr) insert(e, "\n", 1);
            e->paste_cr = 0;
        } else {
            e->paste_cr = 0;
            if (byte >= 32 && byte != 127) insert_character(e, byte);
            else if (byte == '\t') insert(e, "\t", 1);
        }
        return CLIENT_EDITOR_EDIT;
    } else if (byte == '\n') insert(e, "\n", 1);
    else if (byte == 127 || byte == 8) remove_range(e, previous_char(e, e->cursor), e->cursor);
    else if (byte >= 32 && byte != 127) insert_character(e, byte);
    else if (byte == '\t') insert(e, "\t", 1);
    if (byte == '\r') {
        if (frame_fits(e->text)) return CLIENT_EDITOR_SUBMIT;
        e->notice = "Message exceeds protocol frame limit; delete text before sending.";
        fputc('\a', stdout);
    }
    return CLIENT_EDITOR_EDIT;
}

static int edit_input(editor_t *e) {
    redraw(e);
    for (;;) {
        if (resized) {
            resized = 0;
            dimensions(e);
            for (int i = 0; i < e->rows; ++i) fputs("\r\n", stdout);
            e->screen_row = e->drawn_rows = 0;
            e->goal_column = -1;
            redraw(e);
        }
        unsigned char byte;
        int result = read_byte(e, &byte, 100);
        if (result == -2) continue;
        if (result <= 0) return result;
        result = feed_key(e, byte);
        if (result == CLIENT_EDITOR_EXIT) return 0;
        if (result == CLIENT_EDITOR_SUBMIT) return 1;
        if (!e->pasting) redraw(e);
    }
}

client_editor_t *client_editor_create(size_t capacity) {
    if (capacity < 2 || capacity > PROTOCOL_MAX_INPUT) { errno = EINVAL; return NULL; }
    editor_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    // 开一个能够接受 4096字节的text空间
    e->text = calloc(capacity, 1);
    e->positions = calloc(capacity, sizeof(*e->positions));

    if (!e->text || !e->positions) { free(e->text); free(e->positions); free(e); return NULL; }
    e->capacity = capacity; e->pending_byte = e->goal_column = -1;
    
    interrupted = 0;
    return e;
}

void client_editor_destroy(client_editor_t *e) {
    if (e) { free(e->text); free(e->positions); free(e); }
}

const char *client_editor_text(const client_editor_t *e) { return e->text; }

void client_editor_clear(client_editor_t *e) {
    e->text[0] = 0; e->length = e->cursor = 0;
    e->goal_column = e->pending_byte = -1;
    e->pasting = e->paste_cr = e->paste_overflow = 0; e->notice = NULL;
}

int client_editor_read_key(client_editor_t *e) {
    unsigned char byte;
    int result = read_byte(e, &byte, 0);
    if (result == -2) return CLIENT_EDITOR_EDIT;
    if (result <= 0) return CLIENT_EDITOR_EXIT;
    return feed_key(e, byte);
}

int client_editor_pending(const client_editor_t *e) { return e->pending_byte >= 0; }

int client_editor_height(client_editor_t *e, int columns, int maximum) {
    e->columns = columns; layout(e);
    int height = e->positions[e->length].row + 1;
    if (height > maximum) height = maximum;
    return height;
}

const char *client_editor_notice(const client_editor_t *e) { return e->notice; }

void client_editor_draw(client_editor_t *e, int first_row, int height) {
    layout(e);
    int cursor_row = e->positions[e->cursor].row;
    int top = cursor_row >= height ? cursor_row - height + 1 : 0;
    for (int i = 0; i < height; ++i) {
        printf("\033[%d;1H", first_row + i);
        render_line(e, top + i);
    }
    printf("\033[%d;%dH", first_row + cursor_row - top, e->positions[e->cursor].column + 1);
}

static int read_stream_input(char *buffer, size_t capacity) {
    for (;;) {
        fputs("you> ", stdout);
        fflush(stdout);
        size_t length = 0;
        int overflow = 0, byte;
        while ((byte = getchar()) != '\n' && byte != EOF) {
            if (length + 1 < capacity) buffer[length++] = (char)byte;
            else overflow = 1;
        }
        if (byte == EOF && ferror(stdin)) return -1;
        if (byte == EOF && length == 0 && !overflow) return 0;
        buffer[length] = '\0';
        if (overflow || !frame_fits(buffer)) {
            fprintf(stderr, "client: input exceeds %zu bytes or protocol frame limit\n", capacity - 1);
            if (byte == EOF) return 0;
            continue;
        }
        return 1;
    }
}

int client_read_input(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity < 2 || capacity > PROTOCOL_MAX_INPUT) {
        errno = EINVAL; return -1;
    }
    const char *term = getenv("TERM");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        (term != NULL && strcmp(term, "dumb") == 0)) return read_stream_input(buffer, capacity);

    /* C.UTF-8 is available on the supported Linux/WSL platforms, including
     * environments whose LC_CTYPE defaults to the ASCII-only C locale. */
    if (setlocale(LC_CTYPE, "") == NULL || MB_CUR_MAX < 4) {
        if (setlocale(LC_CTYPE, "C.UTF-8") == NULL) { errno = EILSEQ; return -1; }
    }
    if (!cleanup_registered) {
        if (atexit(restore_terminal) != 0) { errno = ENOMEM; return -1; }
        cleanup_registered = 1;
    }
    position_t *positions = calloc(capacity, sizeof(*positions));
    if (positions == NULL) return -1;
    editor_t e = {.text = buffer, .capacity = capacity, .positions = positions,
                  .pending_byte = -1, .goal_column = -1};
    buffer[0] = '\0';
    dimensions(&e);

    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
    struct sigaction previous[4], action = {0};
    action.sa_handler = input_signal;
    sigemptyset(&action.sa_mask);
    interrupted = resized = 0;
    int installed = 0, result = -1;
    for (; installed < 4; ++installed) {
        if (sigaction(signals[installed], &action, &previous[installed]) < 0) goto cleanup;
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) < 0) goto cleanup;
    struct termios raw = original_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR | IGNCR | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) goto cleanup;
    terminal_active = 1;
    fputs("\033[?2004h", stdout);
    result = edit_input(&e);
    finish_display(&e);
cleanup: {
        int saved_errno = errno;
        restore_terminal();
        for (int i = 0; i < installed; ++i) (void)sigaction(signals[i], &previous[i], NULL);
        free(positions);
        errno = saved_errno;
    }
    return result;
}
