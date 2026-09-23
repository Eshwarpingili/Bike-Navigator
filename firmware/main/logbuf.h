#ifndef LOGBUF_H
#define LOGBUF_H

/* A few lines of log kept in RAM and shown on the screen, because the board's
 * UART pins do not reach the USB connector. */

#define LOGBUF_LINES 7
#define LOGBUF_WIDTH 48

/* Adds a line (also goes to the UART). */
void logbuf_add(const char *fmt, ...);

/* Copies the lines out, oldest first, into a caller-owned buffer.
 * Returns the number of lines written. */
int logbuf_snapshot(char out[LOGBUF_LINES][LOGBUF_WIDTH + 1]);

/* Bumps whenever a line is added, so the UI can skip redraws. */
unsigned logbuf_version(void);

#endif
