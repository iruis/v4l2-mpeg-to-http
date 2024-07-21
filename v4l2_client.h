#ifndef V4L2_CLIENT_H
#define V4L2_CLIENT_H

struct v4l2_client;
typedef struct v4l2_client v4l2_client_t;
typedef void (*v4l2_client_callback_t)(v4l2_client_t *obj, void *opaque);

void v4l2_device_list();

v4l2_client_t *v4l2_client_create(const char *device);
void v4l2_client_destroy(v4l2_client_t *obj);

/* success: 0 */
int v4l2_client_start(v4l2_client_t *obj);
void v4l2_client_stop(v4l2_client_t *obj);
void v4l2_client_set_callback(v4l2_client_t *obj, v4l2_client_callback_t callback, void *opaque);

void *v4l2_client_get_buffer(v4l2_client_t *obj);
unsigned int v4l2_client_get_buffer_length(v4l2_client_t *obj);
unsigned int v4l2_client_get_buffer_index(v4l2_client_t *obj);
unsigned int v4l2_client_get_buffer_count(v4l2_client_t *obj);

#endif
