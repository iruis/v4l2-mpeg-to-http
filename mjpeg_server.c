#include "mjpeg_server.h"
#include "logging.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
    reference:
        https://github.com/valbok/mjpeg-over-http/blob/master/bin/mjpeg-over-http.cpp
*/
#define MAX_PENDING 5
#define BOUNDARY "mjpeg-over-http-boundary"

struct mjpeg_server
{
    char *bind;
    short port;

    int stop;
    int event;
    int socket;
    int client; // MJPEG를 전송 할 소캣은 단일로 제한
    int pendings[MAX_PENDING]; // 접속 대기
    int founds[MAX_PENDING]; // GET / HTTP/1.1 수신 유무
    int firsts[MAX_PENDING];

    char *buffer;
    unsigned int buffer_length;
    unsigned int buffer_available;

    pthread_t thread;
    sem_t semaphore;
};

/* success: 0 */
static int socket_write(int sock, const char *buffer, ssize_t *length)
{
    ssize_t len = *length;
    ssize_t left = *length;

    *length = 0;

    while (left)
    {
        ssize_t written = send(sock, buffer + (left - len), left, 0);
        if (written <= 0)
        {
            return 1;
        }
        left -= written;
        *length += written;
    }
    return 0;
}

static void mjpeg_server_client_close(mjpeg_server_t *obj)
{
    if (obj == 0 || obj->client == -1)
    {
        return;
    }
    logging("mjpeg client close: %d", obj->client);
    close(obj->client);
    obj->client = -1;
}

static void mjpeg_server_pending_close(mjpeg_server_t *obj, int idx)
{
    if (obj == 0 || obj->pendings[idx] == -1)
    {
        return;
    }
    logging("mjpeg pending close: %d", obj->pendings[idx]);
    close(obj->pendings[idx]);
    obj->pendings[idx] = -1;
    obj->founds[idx] = 0;
    obj->firsts[idx] = 0;
}

static void mjpeg_server_accept(mjpeg_server_t *obj)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int index = 0;
    int count = 0;
    int pendings[MAX_PENDING];
    int founds[MAX_PENDING];
    int firsts[MAX_PENDING];

    memset(&addr, 0, sizeof(addr));
    memset(founds, 0, sizeof(founds));
    memset(firsts, 0, sizeof(firsts));

    for (int i = 0; i < MAX_PENDING; i++)
    {
        pendings[i] = -1;
    }

    // pending에 빈 자리가 없으면 첫번째 소켓 연결 해제
    for (int i = 0; i < MAX_PENDING; i++)
    {
        if (obj->pendings[i] != -1)
        {
            count++;
        }
    }
    if (count == MAX_PENDING)
    {
        mjpeg_server_pending_close(obj, 0);
    }

    // 새로운 연결이 발생하면 pending 배열을 다시 정렬
    for (int i = 0; i < MAX_PENDING; i++)
    {
        if (obj->pendings[i] != -1)
        {
            pendings[index] = obj->pendings[i];
            founds[index] = obj->founds[i];
            firsts[index] = obj->firsts[i];

            index++;
        }
    }

    memcpy(obj->pendings, pendings, sizeof(pendings));
    memcpy(obj->founds, founds, sizeof(founds));
    memcpy(obj->firsts, firsts, sizeof(firsts));

    obj->pendings[index] = accept(obj->socket, (struct sockaddr *)&addr, &addr_len);
    obj->founds[index] = 0;
    obj->firsts[index] = 1;
}

// 일반적으로 클라이언트의 연결이 끊겼을 경우 호출 됨
static void mjpeg_server_client_process(mjpeg_server_t *obj)
{
    int index = -1;
    char buffer[512];

    if (obj == 0 || obj->client == -1)
    {
        return;
    }

    memset(buffer, 0, sizeof(buffer));

    ssize_t len = recv(obj->client, buffer, sizeof(buffer) - 1, 0);
    if (len <= 0)
    {
        logging("mjpeg client closed (ret: %d)", len);
        mjpeg_server_client_close(obj);
        return;
    }
    else
    {
        logging("mjpeg client message: %s", buffer);
    }
}

static void mjpeg_server_pending_process(mjpeg_server_t *obj, int sock)
{
    int index = -1;
    char buffer[512];

    for (int i = 0; i < MAX_PENDING; i++)
    {
        if (obj->pendings[i] == sock)
        {
            index = i;
            break;
        }
    }
    assert(index != -1);
    memset(buffer, 0, sizeof(buffer));

    ssize_t len = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (len <= 0)
    {
        mjpeg_server_pending_close(obj, index);
        return;
    }

    // 첫 라인의 텍스트는 온전하게 수신된다고 가정함
    if (obj->firsts[index] == 1)
    {
        char *ptr = strstr(buffer, "\r\n");
        if (ptr)
        {
            ptr[0] = 0;
            ptr[1] = 0;
            ptr += 2;
        }
        obj->firsts[index] = 0;
        obj->founds[index] = strcmp(buffer, "GET / HTTP/1.1") == 0;

        logging("mjpeg pending: %d, request: %s", sock, buffer);

        if (ptr)
        {
            memmove(buffer, ptr, (ptr - buffer) + 1); // null 문자 포함하여 이동
        }
    }
    // 헤더의 마지막은 온전하게 수신된다고 가정함
    if (strstr(buffer, "\r\n\r\n") == 0)
    {
        return;
    }

    char response_200[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
        "\r\n"
        "--" BOUNDARY "\r\n";

    char response_404[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    char *response = obj->founds[index] ? response_200 : response_404;
    ssize_t length = strlen(response);

    logging("mjpeg pending: %d, response: %d", sock, obj->founds[index] ? 200 : 404);

    if (socket_write(sock, response, &length) != 0)
    {
        mjpeg_server_pending_close(obj, index);
    }
    else if (obj->founds[index] == 0)
    {
        mjpeg_server_pending_close(obj, index);
    }
    else
    {
        if (obj->client != -1)
        {
            // 기존 연결 해제 후 새로운 소켓으로 변경
            logging("mjpeg client close by other connection");
            mjpeg_server_client_close(obj);
        }
        logging("mjpeg pending %d to client", sock);

        obj->client = sock;
        obj->pendings[index] = -1;
        obj->founds[index] = 0;
    }
}

static void mjpeg_server_client_send_mjpeg(mjpeg_server_t *obj)
{
    char head[128];
    char foot[] = "\r\n--" BOUNDARY "\r\n";
    char message[32];
    ssize_t length;
    ssize_t send_len = 0;

    if (obj == 0 || obj->client == -1 || obj->buffer_length == 0)
    {
        return;
    }

    length = sprintf(head,
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        obj->buffer_length
    );
    assert(sizeof(head) > length);
    if (socket_write(obj->client, head, &length) != 0)
    {
        mjpeg_server_client_close(obj);
        return;
    }
    send_len += length;

    length = obj->buffer_length;
    if (socket_write(obj->client, obj->buffer, &length) != 0)
    {
        mjpeg_server_client_close(obj);
        return;
    }
    send_len += length;

    length = strlen(foot);
    if (socket_write(obj->client, foot, &length) != 0)
    {
        mjpeg_server_client_close(obj);
        return;
    }
    send_len += length;
}

static void *mjpeg_server_main(void *args)
{
    // 0: 이벤트 FD
    // 1: 서버 소켓
    // 2: MJPEG 전송 클라이언트
    // 3~ : 연결 중인 클라이언트
    // MJPEG 전송 클라이언트가 없을 경우 2번 부터 연결 중인 클라이언트
    struct pollfd fds[3 + MAX_PENDING];
    struct mjpeg_server *mjpeg_server = args;

    int ret;
    int count;

    while (1)
    {
        fds[0].fd = mjpeg_server->event;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = mjpeg_server->socket;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        count = 2;

        if (mjpeg_server->client != -1)
        {
            fds[count].fd = mjpeg_server->client;
            fds[count].events = POLLIN;
            fds[count].revents = 0;

            count++;
        }

        for (int i = 0; i < MAX_PENDING; i++)
        {
            if (mjpeg_server->pendings[i] == -1)
            {
                continue;
            }
            fds[count].fd = mjpeg_server->pendings[i];
            fds[count].events = POLLIN;
            fds[count].revents = 0;

            count++;
        }

        ret = poll(fds, count, -1);

        if (ret == -1)
        {
            break;
        }
        if (mjpeg_server->stop)
        {
            break;
        }

        if (fds[0].revents)
        {
            uint64_t u = 0;

            sem_wait(&mjpeg_server->semaphore);
            read(mjpeg_server->event, &u, sizeof(u));

            mjpeg_server_client_send_mjpeg(mjpeg_server);
            sem_post(&mjpeg_server->semaphore);
        }
        if (fds[1].revents)
        {
            mjpeg_server_accept(mjpeg_server);
        }
        for (int i = 2; i < count; i++)
        {
            if (fds[i].revents == 0)
            {
                continue;
            }
            if (fds[i].fd == mjpeg_server->client)
            {
                mjpeg_server_client_process(mjpeg_server);
            }
            else
            {
                mjpeg_server_pending_process(mjpeg_server, fds[i].fd);
            }
        }
    }
    return 0;
}

mjpeg_server_t *mjpeg_server_create(const char *bind, short port)
{
    mjpeg_server_t *obj;
    if (bind == 0)
    {
        return 0;
    }    
    obj = malloc(sizeof(*obj));
    if (obj == 0)
    {
        return 0;
    }
    memset(obj, 0, sizeof(*obj));
    sem_init(&obj->semaphore, 0, 1);
    obj->event = -1;
    obj->socket = -1;
    obj->client = -1;
    for (int i = 0; i < MAX_PENDING; i++)
    {
        obj->pendings[i] = -1;
    }
    obj->port = port;
    obj->bind = strdup(bind);
    if (obj->bind == 0)
    {
        mjpeg_server_destroy(obj);
        return 0;
    }
    return obj;
}

void mjpeg_server_destroy(mjpeg_server_t *obj)
{
    if (obj == 0)
    {
        return;
    }
    if (obj->bind)
    {
        free(obj->bind);
        obj->bind = 0;
    }
    mjpeg_server_stop(obj);

    if (obj->buffer)
    {
        free(obj->buffer);
        obj->buffer = 0;
        obj->buffer_available = 0;
    }

    sem_destroy(&obj->semaphore);
    free(obj);
}

/* success: 0 */
int mjpeg_server_start(mjpeg_server_t *obj)
{
    struct sockaddr_in addr;

    if (obj->event != -1)
    {
        return 1;
    }

    obj->stop = 0;
    obj->event = eventfd(0, 0);
    if (obj->event == -1)
    {
        mjpeg_server_stop(obj);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(obj->bind);
    addr.sin_port = htons(obj->port);

    obj->socket = socket(PF_INET, SOCK_STREAM, 0);
    if (obj->socket == -1)
    {
        perror("socket");
        mjpeg_server_stop(obj);
        return 1;
    }
    int optval = 1;
    if (setsockopt(obj->socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))!= 0)
    {
        perror("setsockopt");
        mjpeg_server_stop(obj);
        return 1;
    }
    if (bind(obj->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        perror("bind");
        mjpeg_server_stop(obj);
        return 1;
    }
    if (listen(obj->socket, 1) != 0)
    {
        perror("listen");
        mjpeg_server_stop(obj);
        return 1;
    }
    if (pthread_create(&obj->thread, 0, &mjpeg_server_main, obj) != 0)
    {
        mjpeg_server_stop(obj);
        return 1;
    }
    return 0;
}

void mjpeg_server_stop(mjpeg_server_t *obj)
{
    if (obj == 0)
    {
        return;
    }
    if (obj->thread)
    {
        uint64_t u = 1;

        obj->stop = 1;

        write(obj->event, &u, sizeof(u));

        pthread_join(obj->thread, 0);
        obj->thread = 0;
    }
    for (int i = 0; i < MAX_PENDING; i++)
    {
        mjpeg_server_pending_close(obj, i);
    }
    if (obj->client != -1)
    {
        mjpeg_server_client_close(obj);
    }
    if (obj->socket != -1)
    {
        close(obj->socket);
        obj->socket = -1;
    }
    if (obj->event != -1)
    {
        close(obj->event);
        obj->event = -1;
    }
}

void mjpeg_server_post(mjpeg_server_t *obj, char *buffer, unsigned int length)
{
    char *ptr;
    uint64_t u = 1;

    if (obj == 0)
    {
        return;
    }
    sem_wait(&obj->semaphore);

    if (obj->buffer == 0)
    {
        obj->buffer = malloc(length);
        obj->buffer_available = length;
    }
    else if (obj->buffer_available < length)
    {
        ptr = obj->buffer;

        obj->buffer = realloc(obj->buffer, length);
        obj->buffer_available = length;

        if (obj->buffer == 0)
        {
            obj->buffer_available = 0;

            free(ptr);
        }
    }

    if (obj->buffer && obj->buffer_available >= length)
    {
        obj->buffer_length = length;

        memcpy(obj->buffer, buffer, length);
        write(obj->event, &u, sizeof(u));
    }
    else
    {
        obj->buffer_length = 0;
    }

    sem_post(&obj->semaphore);
}
