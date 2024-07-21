#ifndef MJPEG_SERVER_H
#define MJPEG_SERVER_H

struct mjpeg_server;
typedef struct mjpeg_server mjpeg_server_t;

mjpeg_server_t *mjpeg_server_create(const char *bind, short port);
void mjpeg_server_destroy(mjpeg_server_t *obj);

/* success: 0 */
int mjpeg_server_start(mjpeg_server_t *obj);
void mjpeg_server_stop(mjpeg_server_t *obj);

void mjpeg_server_post(mjpeg_server_t *obj, char *buffer, unsigned int length);

#endif
