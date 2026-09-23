#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static char g_lines[LOGBUF_LINES][LOGBUF_WIDTH + 1];
static int g_count;   /* lines written so far, may exceed LOGBUF_LINES */
static unsigned g_version;

void logbuf_add(const char *fmt, ...)
{
    char line[LOGBUF_WIDTH + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    taskENTER_CRITICAL();
    strcpy(g_lines[g_count % LOGBUF_LINES], line);
    g_count++;
    g_version++;
    taskEXIT_CRITICAL();

    printf("%s\r\n", line);
}

int logbuf_snapshot(char out[LOGBUF_LINES][LOGBUF_WIDTH + 1])
{
    taskENTER_CRITICAL();
    int total = g_count < LOGBUF_LINES ? g_count : LOGBUF_LINES;
    int first = g_count < LOGBUF_LINES ? 0 : g_count % LOGBUF_LINES;
    for (int i = 0; i < total; i++) {
        strcpy(out[i], g_lines[(first + i) % LOGBUF_LINES]);
    }
    taskEXIT_CRITICAL();
    return total;
}

unsigned logbuf_version(void)
{
    return g_version;
}
