// Asgn 2: A simple HTTP server.
// By: Eugene Chou
//     Andrew Quinn
//     Brian Zhao

#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "response.h"
#include "request.h"
#include "queue.h"

#include <pthread.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>

void handle_connection(int);

void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);

pthread_mutex_t mutex;

queue_t *queue;
void *thread();
int main(int argc, char **argv) {
    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int opt = 0;
    int t = 4;

    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't': t = atoi(optarg); break;
        default: break;
        }
    }

    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[optind]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    queue = queue_new(t);
    pthread_t threads[t];
    pthread_mutex_init(&mutex, NULL);
    for (int x = 0; x < t; x++) {
        pthread_create(&threads[x], NULL, thread, NULL);
    }
    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        queue_push(queue, (void *) connfd);
    }
    return EXIT_SUCCESS;
}

void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        debug("%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }
    conn_delete(&conn);
}

void handle_get(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    debug("GET request not implemented. But, we want to get %s", uri);
    int statusCode = 0;

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        debug("%s: %d", uri, errno);
        if (errno == EACCES) {
            res = &RESPONSE_FORBIDDEN;
            statusCode = 403;
            goto out;
        } else if (errno == ENOENT) {
            res = &RESPONSE_NOT_FOUND;
            statusCode = 404;
            goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            statusCode = 500;
            goto out;
        }
    }
    flock(fd, LOCK_SH);

    struct stat fileContents;
    fstat(fd, &fileContents);
    // Get the size of the file.
    off_t size = fileContents.st_size;

    // Check if the file is a directory, because directories *will*
    if (S_ISDIR(fileContents.st_mode)) {
        res = &RESPONSE_FORBIDDEN;
        statusCode = 403;
        goto out;
    }
    // Send the file
    res = conn_send_file(conn, fd, size);
    if (res == NULL) {
        res = &RESPONSE_OK;
        statusCode = 200;
    }
out:
    if (statusCode == 200) {
        char *reqid = conn_get_header(conn, "Request-Id");
        if (reqid == NULL) {
            reqid = "0";
        }
        fprintf(stderr, "GET,/%s,%d,%s\n", uri, statusCode, reqid);
    } else if (statusCode == 403) {
        conn_send_response(conn, res);
        char *reqid = conn_get_header(conn, "Request-Id");
        if (reqid == NULL) {
            reqid = "0";
        }
        fprintf(stderr, "GET,/%s,%d,%s\n", uri, statusCode, reqid);
    } else if (statusCode == 404) {
        conn_send_response(conn, res);
        char *reqid = conn_get_header(conn, "Request-Id");
        if (reqid == NULL) {
            reqid = "0";
        }
        fprintf(stderr, "GET,/%s,%d,%s\n", uri, statusCode, reqid);
    } else if (statusCode == 500) {
        conn_send_response(conn, res);
        char *reqid = conn_get_header(conn, "Request-Id");
        if (reqid == NULL) {
            reqid = "0";
        }
        fprintf(stderr, "GET,/%s,%d,%s\n", uri, statusCode, reqid);
    }
    close(fd);
}

void handle_unsupported(conn_t *conn) {
    debug("handling unsupported request");

    // send responses
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void *thread() {
    while (1) {
        uintptr_t connfd;
        queue_pop(queue, (void **) &connfd);

        handle_connection(connfd);
        close(connfd);
    }
}

void handle_put(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    debug("handling put request for %s", uri);
    // Check if file already exists before opening it.
    pthread_mutex_lock(&mutex);
    bool existed = access(uri, F_OK) == 0;
    debug("%s existed? %d", uri, existed);

    // Open the file..
    int fd = open(uri, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        debug("%s: %d", uri, errno);
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;

            goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            goto out;
        }
    }
    flock(fd, LOCK_EX);
    pthread_mutex_unlock(&mutex);
    ftruncate(fd, 0);
    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
    }

out:
    conn_send_response(conn, res);
    uint16_t code = response_get_code(res);
    char *reqid = conn_get_header(conn, "Request-Id");
    if (reqid == NULL) {
        reqid = "0";
    }
    if (code == 200) {
        fprintf(stderr, "PUT,/%s,%d,%s\n", uri, code, reqid);
    } else if (code == 201) {
        fprintf(stderr, "PUT,/%s,%d,%s\n", uri, code, reqid);
    } else if (code == 403) {
        fprintf(stderr, "PUT,/%s,%d,%s\n", uri, code, reqid);
    } else if (code == 404) {
        fprintf(stderr, "PUT,/%s,%d,%s\n", uri, code, reqid);
    } else if (code == 500) {
        fprintf(stderr, "PUT,/%s,%d,%s\n", uri, code, reqid);
    }
    close(fd);
}

