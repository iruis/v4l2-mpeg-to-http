#include "v4l2_client.h"
#include "logging.h"

#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <linux/videodev2.h>

struct v4l2_client
{
    char *device;

    int stop;
    int event;
    int fd;
    int on;

    int buf_count;
    void **buf_start;
    unsigned int *buf_len;
    unsigned int buf_index;
    unsigned int buf_bytes;

    void *opaque;
    v4l2_client_callback_t callback;

    pthread_t thread;
};

static const char *format_name(uint32_t format)
{
    switch (format)
    {
    case V4L2_PIX_FMT_MJPEG:
        return "Motion-JPEG";
    //case V4L2_PIX_FMT_VYUY:
    //    return "VYUY 4:2:2";
    case V4L2_PIX_FMT_YUYV:
        return "YUYV 4:2:2";
    //case V4L2_PIX_FMT_YVYU:
    //    return "YVYU 4:2:2";
    //case V4L2_PIX_FMT_UYVY:
    //    return "UYVY 4:2:2";
    default:
        return "Unknown";
    }
}

static int is_v4l_dev(const char *name)
{
    if (strncmp(name, "video", 5) == 0)
    {
        return 1;
    }
    if (strncmp(name, "radio", 5) == 0)
    {
        return 1;
    }
    if (strncmp(name, "vbi", 3) == 0)
    {
        return 1;
    }
    if (strncmp(name, "v4l-subdev", 10) == 0)
    {
        return 1;
    }
    return 0;
}

static void v4l2_device_dump(const char *name)
{
    char device[128];
    sprintf(device, "/dev/%s", name);

    struct v4l2_capability capabililty;
    int fd = open(device, O_RDWR);
    if (fd < 0)
    {
        return;
    }
    if (ioctl(fd, VIDIOC_QUERYCAP, &capabililty) < 0)
    {
        close(fd);
        return;
    }

    // check is capture device
    if ((capabililty.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
        close(fd);
        return;
    }

    printf("device: %s\n", device);
    printf(" - card: %s\n", capabililty.card);
    printf(" - capture: %d\n", (capabililty.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? 1 : 0);
    printf(" - readwrite: %d\n", (capabililty.capabilities & V4L2_CAP_READWRITE) ? 1 : 0);
    printf(" - streaming: %d\n", (capabililty.capabilities & V4L2_CAP_STREAMING) ? 1 : 0);
    printf(" - capabilities: 0x%08x\n", capabililty.capabilities);
    printf(" - device caps: 0x%08x\n", capabililty.device_caps);

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0)
    {
        close(fd);
        return;
    }
    printf(" - default frame size: %ux%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    struct v4l2_fmtdesc vfd = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    while (ioctl(fd, VIDIOC_ENUM_FMT, &vfd) == 0)
    {
        vfd.index++;

        char *pixel_type = (vfd.flags & V4L2_FMT_FLAG_COMPRESSED) ? "compressed" : "       raw";
        printf("   - %s: %11s(0x%08X), %20s :", pixel_type, format_name(vfd.pixelformat), vfd.pixelformat, vfd.description);

        struct v4l2_frmsizeenum vfse = { .pixel_format = vfd.pixelformat };
        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &vfse) == 0)
        {
            switch (vfse.type)
            {
            case V4L2_FRMSIZE_TYPE_DISCRETE:
                printf(" %ux%u", vfse.discrete.width, vfse.discrete.height);
                break;
            case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            case V4L2_FRMSIZE_TYPE_STEPWISE:
                printf(" {%u-%u, %u}x{%u-%u, %u}", vfse.stepwise.min_width, vfse.stepwise.max_width, vfse.stepwise.step_width, vfse.stepwise.min_height, vfse.stepwise.max_height, vfse.stepwise.step_height);
                break;
            }
            vfse.index++;
        }
        printf("\n");
    }
    close(fd);
}

void v4l2_device_list()
{
    DIR *dir = opendir("/dev");
    if (!dir)
    {
        perror("opendir");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        if (!is_v4l_dev(entry->d_name))
        {
            continue;
        }
        v4l2_device_dump(entry->d_name);
    }
    closedir(dir);
}

v4l2_client_t *v4l2_client_create(const char *device)
{
    v4l2_client_t *obj;
    if (device == 0)
    {
        return 0;
    }
    obj = malloc(sizeof(*obj));
    if (obj == 0)
    {
        return 0;
    }
    memset(obj, 0, sizeof(*obj));
    obj->fd  = -1;
    obj->event = -1;
    obj->device = strdup(device);
    if (obj->device == 0)
    {
        v4l2_client_destroy(obj);
        return 0;
    }
    return obj;
}

void v4l2_client_destroy(v4l2_client_t *obj)
{
    if (obj == 0)
    {
        return;
    }
    if (obj->device)
    {
        free(obj->device);
        obj->device = 0;
    }
    v4l2_client_stop(obj);

    free(obj);
}

/* success: 0 */
static int v4l2_client_open(v4l2_client_t *obj)
{
    int fd;
    int failed = 0;
    void **buf_start = 0;
    unsigned int *buf_len = 0;

    if (obj->fd != -1)
    {
        return 1;
    }

    fd = open(obj->device, O_RDWR);
    if (fd == -1)
    {
        return 1;
    }

    struct v4l2_format fmt =
    {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt =
        {
            .pix =
            {
                .width = 1920,
                .height = 1080,
                .pixelformat = V4L2_PIX_FMT_MJPEG,
                .field = V4L2_FIELD_ANY,
            },
        },
    };

    if(ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        close(fd);
        return 1;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
    {
        close(fd);
        return 1;
    }

    // mmap init
    struct v4l2_requestbuffers req =
    {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .count = 256,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    {
        close(fd);
        return 1;
    }
    if (req.count == 0)
    {
        close(fd);
        return 1;
    }

    buf_start = malloc(req.count * sizeof(void *));
    buf_len = malloc(req.count * sizeof(unsigned int));
    if (buf_start == 0 || buf_len == 0)
    {
        if (buf_start)
        {
            free(buf_start);
        }
        if (buf_len)
        {
            free(buf_len);
        }
        close(fd);
        return 1;
    }
    for (unsigned int i = 0; i < req.count; i++)
    {
        buf_start[i] = MAP_FAILED;
    }
    for (unsigned int i = 0; i < req.count; i++)
    {
        struct v4l2_buffer buf =
        {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .index = i,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            failed = 1;
            break;
        }

        buf_len[i] = buf.length;
        buf_start[i] = mmap(0, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buf_start[i] == MAP_FAILED)
        {
            failed = 1;
            break;
        }
    }
    // end of mmap init

    if (failed == 0)
    {
        struct v4l2_buffer buf =
        {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        for (unsigned int i = 0; i < req.count; i++)
        {
            buf.index = i;

            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
            {
                failed = 1;
                break;
            }
        }
    }
    if (failed == 0)
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type))
        {
            failed = 1;
        }
    }

    if (failed == 0)
    {
        logging("v4l2 size: %ix%i", fmt.fmt.pix.width, fmt.fmt.pix.height);
        logging("v4l2 pixel format: %s, interlaced: %c", format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.field & V4L2_FIELD_INTERLACED ? 'Y' : 'N');
        logging("v4l2 buffer count: %u", req.count);

        obj->buf_count = req.count;
        obj->buf_start = buf_start;
        obj->buf_len = buf_len;
        obj->fd = fd;

        return 0;
    }
    else
    {
        for (unsigned int i = 0; i < req.count; i++)
        {
            if (buf_start[i] != MAP_FAILED)
            {
                munmap(buf_start[i], buf_len[i]);
            }
        }
        free(buf_start);
        free(buf_len);
        close(fd);
        return 1;
    }
}

void v4l2_client_close(v4l2_client_t *obj)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (obj->fd == -1)
    {
        return;
    }

    ioctl(obj->fd, VIDIOC_STREAMOFF, &type);

    for (unsigned int i = 0; i < obj->buf_count; i++)
    {
        munmap(obj->buf_start[i], obj->buf_len[i]);
    }
    free(obj->buf_start);
    free(obj->buf_len);
    close(obj->fd);

    obj->buf_count = 0;
    obj->buf_start = 0;
    obj->buf_len = 0;
    obj->fd = -1;
}

void *v4l2_client_reader(void *arg)
{
    v4l2_client_t *obj = arg;

    while (obj->stop == 0)
    {
        struct v4l2_buffer buf =
        {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(obj->fd, VIDIOC_DQBUF, &buf) < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            break;
        }
        obj->buf_index = buf.index;
        obj->buf_bytes = buf.bytesused;

        if (obj->callback)
        {
            obj->callback(obj, obj->opaque);
        }

        if (ioctl(obj->fd, VIDIOC_QBUF, &buf) < 0)
        {
            break;
        }
    }
    logging("v4l2 stopped");
}

/* success: 0 */
int v4l2_client_start(v4l2_client_t *obj)
{
    if (obj->event != -1)
    {
        return 1;
    }

    obj->stop = 0;
    obj->event = eventfd(0, 0);
    if (obj->event == -1)
    {
        v4l2_client_stop(obj);
        return 1;
    }
    if (v4l2_client_open(obj))
    {
        v4l2_client_stop(obj);
        return 1;
    }
    if (pthread_create(&obj->thread, 0, &v4l2_client_reader, obj) != 0)
    {
        v4l2_client_stop(obj);
        return 1;
    }
    return 0;
}

void v4l2_client_stop(v4l2_client_t *obj)
{
    if (obj->thread)
    {
        uint64_t u = 1;

        obj->stop = 1;

        write(obj->event, &u, sizeof(u));

        pthread_join(obj->thread, 0);
        obj->thread = 0;
    }
    if (obj->event != -1)
    {
        close(obj->event);
        obj->event = -1;
    }
    v4l2_client_close(obj);
}

void v4l2_client_set_callback(v4l2_client_t *obj, v4l2_client_callback_t callback, void *opaque)
{
    obj->callback = callback;
    obj->opaque = opaque;
}

void *v4l2_client_get_buffer(v4l2_client_t *obj)
{
    if (obj->buf_start == 0)
    {
        return 0;
    }
    return obj->buf_start[obj->buf_index];
}

unsigned int v4l2_client_get_buffer_length(v4l2_client_t *obj)
{
    return obj->buf_bytes;
}

unsigned int v4l2_client_get_buffer_index(v4l2_client_t *obj)
{
    return obj->buf_index;
}

unsigned int v4l2_client_get_buffer_count(v4l2_client_t *obj)
{
    return obj->buf_count;
}
