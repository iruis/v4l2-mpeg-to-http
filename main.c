#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "logging.h"
#include "mjpeg_server.h"
#include "v4l2_client.h"

static int done = 0;
static int event = -1;
static void signal_handler(int sig)
{
    uint64_t u = 1;

    if (sig == SIGINT && event >= 0)
    {
        done = 1;

        write(event, &u, sizeof(u));
    }
}

static void v4l2_client_callback(v4l2_client_t *obj, void *opaque)
{
    void *buffer = v4l2_client_get_buffer(obj);
    //unsigned int index = v4l2_client_get_buffer_index(obj);
    unsigned int length = v4l2_client_get_buffer_length(obj);

    mjpeg_server_post(opaque, buffer, length);
    //char message[128];
    //sprintf(message, "read queue: %3i, pointer: %p, length: %6i\r", index, buffer, length);
    //write(fileno(stdout), message, strlen(message));
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/video0";

    logging_init();

    if (argc >= 2 && strcmp(argv[1], "-l") == 0)
    {
        v4l2_device_list();

        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "-d") == 0)
    {
        device = argv[2];
    }

    if (signal(SIGINT, signal_handler) == SIG_ERR)
    {
        return 1;
    }

    event = eventfd(0, 0);
    if (event == -1)
    {
        return 1;
    }

    int v4l2_ret;
    int mjpeg_ret;

    v4l2_client_t *v4l2 = v4l2_client_create(device);
    mjpeg_server_t *mjpeg = mjpeg_server_create("0.0.0.0", 8080);
    
    if (v4l2 == 0 || mjpeg == 0)
    {
        v4l2_client_destroy(v4l2);
        mjpeg_server_destroy(mjpeg);

        return 1;
    }

    v4l2_client_set_callback(v4l2, v4l2_client_callback, mjpeg);

    v4l2_ret = v4l2_client_start(v4l2);
    mjpeg_ret = mjpeg_server_start(mjpeg);

    logging("v4l2: %d, mjpeg: %d\n", v4l2_ret, mjpeg_ret);

    if (v4l2_ret == 0 && mjpeg_ret == 0)
    {
        uint64_t u = 0;

        read(event, &u, sizeof(u));

        v4l2_client_stop(v4l2);
        mjpeg_server_stop(mjpeg);
    }

    v4l2_client_destroy(v4l2);
    mjpeg_server_destroy(mjpeg);
    close(event);
    return 0;
}
