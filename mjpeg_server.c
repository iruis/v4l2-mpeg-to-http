#include "mjpeg_server.h"
#include "logging.h"

#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
    reference:
        https://github.com/valbok/mjpeg-over-http/blob/master/bin/mjpeg-over-http.cpp
*/
#define MAX_CLIENT 5
#define BOUNDARY "mjpeg-over-http-boundary"

enum socket_state
{
    none,
    read_get,
    read_head,
    send_mjpeg,
    send_favicon,
};

enum http_version
{
    http_v1_0,
    http_v1_1,
};

struct mjpeg_buffer
{
    char *data;
    unsigned int length;
    unsigned int available;
};

struct mjpeg_socket
{
    int id;
    int code;
    int event_data;
    // 주로 클라이언트 스레드에서 문제가 있어서 작업을 종료할 때 write
    // 서버 스레드와 클라이언트 스레드에서 poll 함수로 종료 확인
    // 서버 스레드에서 클라이언트를 정리하기 위해 클라이언트 스레드에서는 read를 하지 말아야 함
    int event_stop;
    // 클라이언트 스레드에서 소켓 오류 발생 시 해제 시키므로
    // 서버 스레드에서는 클라이언트 스레드가 종료된 후 체크하여 해제를 해야 함
    int socket;

    char path[256];

    enum socket_state state;
    enum http_version version;

    struct mjpeg_buffer buffer;

    pthread_t thread;
    sem_t semaphore;
};

struct mjpeg_server
{
    char *bind;
    short port;

    int stop;
    int event;
    int socket;

    struct mjpeg_buffer buffer;
    struct mjpeg_socket *clients;

    pthread_t thread;
    sem_t semaphore;
};

static void *mjpeg_client_thread(void *arg);

static struct mjpeg_buffer *read_favicon()
{
    char path[256];
    off_t size = 0;

    struct mjpeg_buffer *buffer = 0;

    if (readlink("/proc/self/exe", path, sizeof(path)) != -1)
    {
        char *str = strrchr(path, '/');
        if (str)
        {
            str[0] = 0;
        }
        strcpy(path + strlen(path), "/favicon.ico");

        struct stat st;
        if (stat(path, &st) != -1)
        {
            size = st.st_size;

            logging("favicon(path: %s, size: %d)", path, size);
        }
    }

    if (!size)
    {
        return 0;
    }

    buffer = malloc(sizeof(*buffer));
    if (!buffer)
    {
        return 0;
    }
    buffer->data = malloc(size);
    buffer->length = size;
    buffer->available = size;
    if (!buffer->data)
    {
        free(buffer);
        return 0;
    }
    
    int fd = open(path, O_RDONLY);
    if (fd == -1)
    {
        free(buffer->data);
        free(buffer);
    }

    size_t left = size;
    while (left > 0)
    {
        size_t readlen = read(fd, buffer->data + (size - left), left);
        if (readlen <= 0)
        {
            free(buffer->data);
            free(buffer);
            close(fd);
            return 0;
        }
        left -= readlen;
    }
    return buffer;
}

/* success: 0 */
static int prepare_buffer(struct mjpeg_buffer *buffer, unsigned int size)
{
    if (!buffer)
    {
        return 1;
    }
    if (buffer->data && buffer->available >= size)
    {
        return 0;
    }
    if (buffer->data)
    {
        void *data = buffer->data;

        buffer->data = realloc(buffer->data, size);

        if (!buffer->data)
        {
            free(data);
        }
    }
    else
    {
        buffer->data = malloc(size);
    }

    if (buffer->data)
    {
        buffer->available = size;

        return 0;
    }

    buffer->length = 0;
    buffer->available = 0;

    return 1;
}

/* success: 0 */
static int socket_write(int sock, const char *buffer, ssize_t *length)
{
    ssize_t len = *length;
    ssize_t left = *length;

    *length = 0;

    while (left)
    {
        ssize_t written = send(sock, buffer + (left - len), left, MSG_NOSIGNAL);
        if (written <= 0)
        {
            return 1;
        }
        left -= written;
        *length += written;
    }
    return 0;
}

static void mjpeg_server_client_close(mjpeg_server_t *obj, int idx)
{
    void **rc = 0;
    uint64_t u = 1;

    if (obj == 0)
    {
        return;
    }
    struct mjpeg_socket *client = &obj->clients[idx];

    logging("mjpeg client close: (event_data: %d, event_stop: %d, socket: %d)", client->event_data, client->event_stop, client->socket);

    if (client->event_stop != -1)
    {
        write(obj->clients[idx].event_stop, &u, sizeof(u));
    }
    if (obj->clients[idx].thread)
    {
        pthread_join(obj->clients[idx].thread, rc);
    }
    if (obj->clients[idx].event_stop != -1)
    {
        close(obj->clients[idx].event_stop);
    }
    if (obj->clients[idx].event_data != -1)
    {
        close(obj->clients[idx].event_data);
    }
    if (obj->clients[idx].socket != -1)
    {
        close(obj->clients[idx].socket);
    }
    obj->clients[idx].event_data = -1;
    obj->clients[idx].event_stop = -1;
    obj->clients[idx].socket = -1;
    obj->clients[idx].state = none;
    obj->clients[idx].thread = 0;

    if (idx == MAX_CLIENT - 1)
    {
        return;
    }

    struct mjpeg_socket current = obj->clients[idx];

    memmove(&obj->clients[idx], &obj->clients[idx + 1], sizeof(struct mjpeg_socket) * (MAX_CLIENT - idx - 1));

    obj->clients[MAX_CLIENT - 1] = current;
}

static void mjpeg_server_accept(mjpeg_server_t *obj)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int index = 0;
    int count = 0;

    memset(&addr, 0, sizeof(addr));

    // pending에 빈 자리가 없으면 첫번째 소켓 연결 해제
    logging("check open socket");
    for (int i = 0; i < MAX_CLIENT; i++)
    {
        logging(" - idx: %d, socket: %d", i, obj->clients[i].socket);
        if (obj->clients[i].socket != -1)
        {
            count++;
        }
    }
    if (count == MAX_CLIENT)
    {
        mjpeg_server_client_close(obj, 0);
    }

    // 첫번째 빈 소켓 탐색
    for (int i = 0; i < MAX_CLIENT; i++)
    {
        if (obj->clients[i].socket == -1)
        {
            index = i;
            break;
        }
    }
    logging("using clients index: %d", index);

    struct mjpeg_socket *client = &obj->clients[index];

    // eventfd를 생성 못하면 리턴
    client->event_data = eventfd(0, 0);
    client->event_stop = eventfd(0, 0);
    if (client->event_data == -1 || client->event_stop == -1)
    {
        if (client->event_data != -1)
        {
            close(client->event_data);
        }
        if (client->event_stop != -1)
        {
            close(client->event_stop);
        }
        client->event_data = -1;
        client->event_stop = -1;
        return;
    }

    obj->clients[index].code = 0;
    obj->clients[index].socket = accept(obj->socket, (struct sockaddr *)&addr, &addr_len);
    obj->clients[index].state = read_get;

    if (obj->clients[index].buffer.data)
    {
        obj->clients[index].buffer.data[0] = 0;
        obj->clients[index].buffer.length = 0;
    }
    else
    {
        obj->clients[index].buffer.length = 0;
        obj->clients[index].buffer.available = 0;
    }

    if (pthread_create(&obj->clients[index].thread, 0, mjpeg_client_thread, &obj->clients[index]) != 0)
    {
        mjpeg_server_client_close(obj, index);
    }
}

static void mjpeg_server_client_post_mjpeg(mjpeg_server_t *obj)
{
    uint64_t u = 1;

    for (int i = 0; i < MAX_CLIENT; i++)
    {
        struct mjpeg_socket *client = &obj->clients[i];

        if (obj == 0 || client->event_data == -1)
        {
            return;
        }
        if (client->state != send_mjpeg)
        {
            continue;
        }

        sem_wait(&client->semaphore);
        if (prepare_buffer(&client->buffer, obj->buffer.length) == 0)
        {
            client->buffer.length = obj->buffer.length;
            memcpy(client->buffer.data, obj->buffer.data, obj->buffer.length);
            write(client->event_data, &u, sizeof(u));
        }
        sem_post(&client->semaphore);
    }
}

static void *mjpeg_server_main(void *args)
{
    // 0: 이벤트 FD
    // 1: 서버 소켓
    // 2~: 클라이언트
    struct pollfd fds[3 + MAX_CLIENT];
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

        for (int i = 0; i < MAX_CLIENT; i++)
        {
            if (mjpeg_server->clients[i].event_stop == -1)
            {
                continue;
            }
            fds[count].fd = mjpeg_server->clients[i].event_stop;
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
            mjpeg_server_client_post_mjpeg(mjpeg_server);
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
            int idx = -1;
            for (int j = 0; j < MAX_CLIENT; j++)
            {
                if (mjpeg_server->clients[j].event_stop == fds[i].fd)
                {
                    idx = j;
                    break;
                }
            }
            if (idx == -1)
            {
                continue;
            }
            mjpeg_server_client_close(mjpeg_server, idx);
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
    obj->clients = malloc(sizeof(struct mjpeg_socket) * MAX_CLIENT);
    if (!obj->clients)
    {
        free(obj);
        return 0;
    }
    for (int i = 0; i < MAX_CLIENT; i++)
    {
        obj->clients[i].id = i + 1;
        obj->clients[i].event_data = -1;
        obj->clients[i].event_stop = -1;
        obj->clients[i].socket = -1;
        sem_init(&obj->clients[i].semaphore, 0, 1);
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
    void **rc = 0;
    uint64_t u = 1;

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

    for (int i = 0; i < MAX_CLIENT; i++)
    {
        obj->stop = 1;
        if (obj->clients[i].event_stop != -1)
        {
            write(obj->clients[i].event_stop, &u, sizeof(u));
        }
        if (obj->clients[i].thread)
        {
            pthread_join(obj->clients[i].thread, rc);
        }
        if (obj->clients[i].event_data != -1)
        {
            close(obj->clients[i].event_data);
        }
        if (obj->clients[i].event_stop != -1)
        {
            close(obj->clients[i].event_data);
        }
        if (obj->clients[i].socket != -1)
        {
            close(obj->clients[i].socket);
        }
        if (obj->clients[i].buffer.data)
        {
            free(obj->clients[i].buffer.data);
        }
        obj->clients[i].event_data = -1;
        obj->clients[i].event_stop = -1;
        obj->clients[i].socket = -1;
        obj->clients[i].thread = 0;
        obj->clients[i].buffer.data = 0;
        obj->clients[i].buffer.length = 0;
        obj->clients[i].buffer.available = 0;

        sem_destroy(&obj->clients[i].semaphore);
    }
    if (obj->buffer.data)
    {
        free(obj->buffer.data);
    }
    obj->buffer.data = 0;
    obj->buffer.length = 0;
    obj->buffer.available = 0;

    sem_destroy(&obj->semaphore);
    free(obj->clients);
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

    if (obj->buffer.data == 0)
    {
        obj->buffer.data = malloc(length);
        obj->buffer.available = length;
    }
    else if (obj->buffer.available < length)
    {
        ptr = obj->buffer.data;

        obj->buffer.data = realloc(obj->buffer.data, length);
        obj->buffer.available = length;

        if (obj->buffer.data == 0)
        {
            obj->buffer.available = 0;

            free(ptr);
        }
    }

    if (obj->buffer.data && obj->buffer.available >= length)
    {
        obj->buffer.length = length;

        memcpy(obj->buffer.data, buffer, length);

        write(obj->event, &u, sizeof(u));
    }
    else
    {
        obj->buffer.length = 0;
    }

    sem_post(&obj->semaphore);
}

static void mjpeg_client_process_header(struct mjpeg_socket *client)
{
    struct mjpeg_buffer *favicon = 0;

    char buffer[512];
    uint64_t u = 1;

    if (client->socket == -1)
    {
        return;
    }
    memset(buffer, 0, sizeof(buffer));

    ssize_t recvlen = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
    if (recvlen <= 0)
    {
        write(client->event_stop, &u, sizeof(u));
        close(client->socket);
        client->socket = -1;
        return;
    }

    // TODO
    // 헤더를 클라이언트 버퍼에 \r\n\r\n 문자가 수신될 때 까지 append
    // \r\n\r\n 문자가 없으면 리턴하여 다음 수신 대기
    // \r\n\r\n 문자가 있으면 첫번째 라인을 확인하여 HTTP 요청인지 확인 후 클라이언트 state를 send_mjpeg로 변경
    if (client->buffer.data)
    {
        size_t length = strlen(client->buffer.data);

        if (prepare_buffer(&client->buffer, length + recvlen + 1))
        {
            write(client->event_stop, &u, sizeof(u));
            return;
        }
        client->buffer.length += recvlen;

        strncpy(client->buffer.data + length, buffer, recvlen);
    }
    else
    {
        if (prepare_buffer(&client->buffer, recvlen + 1))
        {
            write(client->event_stop, &u, sizeof(u));
            return;
        }
        client->buffer.length = recvlen + 1;

        strncpy(client->buffer.data, buffer, recvlen);
    }
    if (strstr(client->buffer.data, "\r\n") == 0)
    {
        return;
    }

    if (client->state == read_get)
    {
        char method[12];
        char path[256];
        char version[5];

        if (sscanf(client->buffer.data, "%10s %250s HTTP/%4s\r\n", method, path, version) != 3)
        {
            return;
        }

        client->code = strcmp(path, "/") == 0 || strcmp(path, "/video.mjpeg") == 0 || strcmp(path, "/favicon.ico") == 0 ? 200 : 404;
        client->version = strncmp(version, "1.1", 3) == 0 ? http_v1_1 : http_v1_0;
        client->state = read_head;

        strncpy(client->path, path, sizeof(client->path));
        logging("mjpeg pending: %d, request: %s %s HTTP/%s", client->socket, method, path, version);
    }
    if (client->state == read_get)
    {
        logging("mjpeg unknown: %d, data: %d", client->socket, client->buffer);
        return;
    }
    // 헤더의 마지막이 수신되었는지 유무를 확인
    if (strstr(client->buffer.data, "\r\n\r\n") == 0)
    {
        return;
    }

    char response_200[256];
    if (strcmp(client->path, "/favicon.ico"))
    {
        snprintf(
            response_200,
            sizeof(response_200),
            "HTTP/%s 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
            "\r\n"
            "--" BOUNDARY "\r\n",
            client->version == http_v1_0 ? "1.0" : "1.1"
        );
    }
    else
    {
        favicon = read_favicon();

        if (favicon)
        {
            snprintf(
                response_200,
                sizeof(response_200),
                "HTTP/%s 200 OK\r\n"
                "Content-Type: image/x-icon\r\n"
                "Content-Length: %d\r\n"
                "\r\n",
                client->version == http_v1_0 ? "1.0" : "1.1",
                favicon->length
            );
        }
        else
        {
            client->code = 404;
        }
    }

    char *response_404 = (client->version == http_v1_0)
        ?
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "\r\n"
        :
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "\r\n"
        ;

    char *response = client->code == 200 ? response_200 : response_404;
    ssize_t length = strlen(response);

    logging("mjpeg pending: %d, response: %d", client->socket, client->code);

    if (socket_write(client->socket, response, &length))
    {
        write(client->event_stop, &u, sizeof(u));
        close(client->socket);
        client->socket = -1;
    }
    else if (favicon)
    {
        ssize_t length = favicon->length;
        socket_write(client->socket, favicon->data, &length);
        close(client->socket);
        client->socket = -1;

        free(favicon->data);
        free(favicon);
    }
    else
    {
        client->state = send_mjpeg;
    }
}

// 브라우저에서 서버로 데이터를 보낼 때 호출
// 브라우저는 처음 HTTP REQUEST 이후 서버로 데이터를 전송하지 않으므로
// 일반적으로 클라이언트의 연결이 끊겼을 경우 호출 됨
static void mjpeg_client_process_content(struct mjpeg_socket *client)
{
    char buffer[512];
    uint64_t u = 1;

    if (client == 0 || client->socket == -1)
    {
        return;
    }

    memset(buffer, 0, sizeof(buffer));

    ssize_t len = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
    if (len <= 0)
    {
        logging("mjpeg client closed (ret: %d)", len);
        write(client->event_stop, &u, sizeof(u));
        close(client->socket);
        client->socket = -1;
    }
    else
    {
        logging("mjpeg client message (ignore): %s", buffer);
    }
}

static void mjpeg_client_send_data(struct mjpeg_socket *client)
{
    char head[128];
    char foot[] = "\r\n--" BOUNDARY "\r\n";
    ssize_t length;
    ssize_t send_len = 0;
    uint64_t u = 1;

    length = sprintf(head,
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        client->buffer.length
    );
    assert(sizeof(head) > length);
    //printf("%s\n", head);
    if (socket_write(client->socket, head, &length) != 0)
    {
        logging("mjpeg failed header");
        write(client->event_stop, &u, sizeof(u));
        close(client->socket);
        client->socket = -1;
        return;
    }
    send_len += length;

    length = client->buffer.length;
    //printf("... send body\n");
    if (socket_write(client->socket, client->buffer.data, &length) != 0)
    {
        //printf("... socket closed\n");
        logging("mjpeg failed body");
        write(client->event_stop, &u, sizeof(u));
        close(client->socket);
        client->socket = -1;
        return;
    }
    //printf("... send length: %d\n", length);
    send_len += length;

    length = strlen(foot);
    //printf("%s\n", foot);
    if (socket_write(client->socket, foot, &length) != 0)
    {
        logging("mjpeg failed footer");
        close(client->socket);
        client->socket = -1;
        return;
    }
    send_len += length;

    //logging("post socket %d, written %d bytes", client->socket, send_len);
}

static void *mjpeg_client_thread(void *arg)
{
    struct pollfd fds[3];
    struct mjpeg_socket *client = arg;

    int ret;
    uint64_t u = 1;

    logging("start client thread(event_stop: %d, event_data: %d, socket: %d)", client->event_stop, client->event_data, client->socket);

    while (1)
    {
        fds[0].fd = client->event_stop;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = client->event_data;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        fds[2].fd = client->socket;
        fds[2].events = POLLIN;
        fds[2].revents = 0;

        ret = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);
        if (ret == -1)
        {
            write(client->event_stop, &u, sizeof(u));

            break;
        }
        if (fds[0].revents)
        {
            break;
        }
        if (fds[1].revents)
        {
            read(fds[1].fd, &u, sizeof(u));

            if (client->state != send_mjpeg)
            {
                continue;
            }

            sem_wait(&client->semaphore);
            mjpeg_client_send_data(client);
            sem_post(&client->semaphore);
        }
        if (fds[2].revents)
        {
            enum socket_state state = client->state;
            if (state == read_get || state == read_head)
            {
                mjpeg_client_process_header(client);
            }
            else
            {
                logging("received socket: %d", fds[2].fd);
                mjpeg_client_process_content(client);
            }
        }
    }
}
