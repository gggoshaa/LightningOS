#include "kprintf.h"
#include "string.h"
#include "vga.h"
#include "serial.h"

/* Small output sink so kprintf() and ksnprintf() can share the formatter. */
typedef struct {
    char  *buf;      /* NULL => write straight to the console */
    size_t size;
    size_t pos;
} sink_t;

static void sink_putc(sink_t *s, char c)
{
    if (s->buf) {
        if (s->pos + 1 < s->size)
            s->buf[s->pos] = c;
        s->pos++;
    } else {
        vga_putc(c);
        serial_putc(c);
    }
}

static void sink_puts(sink_t *s, const char *str)
{
    while (*str)
        sink_putc(s, *str++);
}

static void format_uint(char *out, uint32_t value, uint32_t base, int upper)
{
    const char *digits_lower = "0123456789abcdef";
    const char *digits_upper = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    char tmp[36];
    int i = 0;

    if (value == 0)
        tmp[i++] = '0';
    while (value) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    int j = 0;
    while (i)
        out[j++] = tmp[--i];
    out[j] = '\0';
}

static void emit_padded(sink_t *s, const char *text, int width, char pad, bool left)
{
    int len = (int)strlen(text);

    if (left) {
        sink_puts(s, text);
        for (int i = len; i < width; i++)
            sink_putc(s, ' ');
        return;
    }
    for (int i = len; i < width; i++)
        sink_putc(s, pad);
    sink_puts(s, text);
}

static void format(sink_t *s, const char *fmt, va_list args)
{
    char scratch[36];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_putc(s, *p);
            continue;
        }

        p++;
        if (*p == '\0')
            break;

        char pad = ' ';
        int width = 0;
        bool left = false;

        if (*p == '-') {
            left = true;
            p++;
        }
        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        switch (*p) {
        case 'c':
            sink_putc(s, (char)va_arg(args, int));
            break;
        case 's': {
            const char *str = va_arg(args, const char *);
            emit_padded(s, str ? str : "(null)", width, ' ', left);
            break;
        }
        case 'd':
        case 'i': {
            int32_t value = va_arg(args, int32_t);
            if (value < 0) {
                sink_putc(s, '-');
                format_uint(scratch, (uint32_t)(-value), 10, 0);
            } else {
                format_uint(scratch, (uint32_t)value, 10, 0);
            }
            emit_padded(s, scratch, width, pad, left);
            break;
        }
        case 'u':
            format_uint(scratch, va_arg(args, uint32_t), 10, 0);
            emit_padded(s, scratch, width, pad, left);
            break;
        case 'x':
            format_uint(scratch, va_arg(args, uint32_t), 16, 0);
            emit_padded(s, scratch, width, pad, left);
            break;
        case 'X':
            format_uint(scratch, va_arg(args, uint32_t), 16, 1);
            emit_padded(s, scratch, width, pad, left);
            break;
        case 'p':
            sink_puts(s, "0x");
            format_uint(scratch, (uint32_t)(uintptr_t)va_arg(args, void *), 16, 0);
            emit_padded(s, scratch, 8, '0', false);
            break;
        case '%':
            sink_putc(s, '%');
            break;
        default:
            sink_putc(s, '%');
            sink_putc(s, *p);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    sink_t sink = { NULL, 0, 0 };
    va_list args;

    va_start(args, fmt);
    format(&sink, fmt, args);
    va_end(args);
}

void kvsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    sink_t sink = { buf, size, 0 };

    format(&sink, fmt, args);
    if (size)
        buf[sink.pos < size ? sink.pos : size - 1] = '\0';
}

void ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    kvsnprintf(buf, size, fmt, args);
    va_end(args);
}
