#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>

static int print_stderr;

void logging_init()
{
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd >= 0)
    {
        close(tty_fd);
        print_stderr = 1;
    }
    else if (errno == ENOENT || errno == EPERM || errno == ENXIO)
    {
        print_stderr = isatty(STDERR_FILENO);
    }
    else
    {
        print_stderr = 1;
    }
}

void logging(const char *format, ...)
{
    va_list args;
    va_start(args, format);

    if (print_stderr)
    {
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    else
    {
        vsyslog(LOG_INFO, format, args);
    }

    va_end(args);
}
