#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <asm/errno.h>
#include <signal.h>

#include "gfserver.h"

#define BUFSIZE 4096
#define SOCKTO 50

static const int dbg = 1;


/***********************/
/* local constants */
/***********************/
static const char *scheme = "GETFILE";
static const char *mthd_get = "GET";
static const char *mthd_head = "HEAD";
static const char *stat_ok = "OK";
static const char *stat_fnf = "FILE_NOT_FOUND";
static const char *stat_er = "ERROR";
static const char *mrkr = "\r\n\r\n";


/****************/
/* thread stuff */
/****************/
static steque_t *rqst_que;
static pthread_mutex_t rqst_lock;
static pthread_cond_t rqst_rdy;
static int g_nthds = 0;


/*************************/
/* function declarations */
/*************************/
static void handler_enqueue_rqst(gfcontext_t *ctx);
static void *handler_dequeue_rqsts(void *arg);
static int gfs_handle_requests(gfcontext_t *ctx);
static gfcontext_t* gfcontext_create(gfserver_t *gfs, struct sockaddr_in* cli_addr, socklen_t cli_addr_len, int sockfd);
static void gfs_init();
static void gfs_cleanup();


/**************************/
/* gfserver API functions */
/**************************/

void gfserver_init(gfserver_t *gfs, unsigned short nwrkr_thds) {
    if (nwrkr_thds > 0) {
        gfs->nwrkr_thds = nwrkr_thds;
    } else {
        gfs->nwrkr_thds = 1;
    }
    g_nthds = nwrkr_thds;
    gfs_init();
}

void gfserver_set_port(gfserver_t *gfs, unsigned short port) {
    gfs->port = port;
}

void gfserver_set_maxpending(gfserver_t *gfs, int max_npending) {
    gfs->max_pend = max_npending;
}

void gfserver_set_handler(gfserver_t *gfs, ssize_t (*handler)(gfcontext_t *, char *, void*)) {
    gfs->hndlr_func = handler;
}

void gfserver_set_handlerarg(gfserver_t *gfs, void *arg) {
    gfs->hndlr_arg = arg;
}

void gfserver_serve(gfserver_t *gfs) {

    /* start worker threads */
    if (dbg) fprintf(stderr, "[INFO] creating threads ... \n");
    pthread_t thrds[gfs->nwrkr_thds];
    for (int ithd = 0; ithd < gfs->nwrkr_thds; ithd++) {
        int rc = pthread_create(&thrds[ithd], NULL, handler_dequeue_rqsts, (void *) &ithd);
        if (rc) {
            fprintf(stderr, "[ERROR] when attempting to create thread %i\n - %d", ithd, rc);
            raise(SIGTERM);
        }
    }

    /* create socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] opening socket");
        raise(SIGTERM);
    }

    /* set socket option to reuse addresses */
    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("[ERROR] setting port reuse option in setsockopt.\n");
        raise(SIGTERM);
    }

    /* set socket timeout options */
    struct timeval timeout;
    timeout.tv_sec = SOCKTO;
    timeout.tv_usec = 0;
    if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
                    sizeof(timeout)) < 0) {
        perror("[ERROR] setting receive timeout in setsockopt\n");
        raise(SIGTERM);
    }

    if (setsockopt (sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                    sizeof(timeout)) < 0) {
        perror("[ERROR] setting send timeout in setsockopt\n");
        raise(SIGTERM);
    }

    /* initialize socket address structure */
    struct sockaddr_in serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(gfs->port);

    /* bind the socket to the host address */
    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[ERROR] binding socket to host address\n");
        raise(SIGTERM);
    }

    /* start accepting requests */
    struct sockaddr_in cli_addr;
    socklen_t clilen;
    int newsockfd;
    listen(sockfd, gfs->max_pend);
    while (1) {

        /* wait for new request */
        clilen = sizeof(cli_addr);
        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0) {
            perror("[ERROR] when accepting client request\n");
            raise(SIGTERM);
        }

        /* create new connection context */
        if (dbg) fprintf(stderr, "[INFO] creating new context\n");
        gfcontext_t *ctx = gfcontext_create(gfs, &cli_addr, clilen, newsockfd);

        /* handle requests for this connection */
        //int stat = gfs_handle_requests(gfs, ctx);
        //if (stat < 0) {
        //    fprintf(stderr, "[ERROR] something went wrong when trying to serve "
        //            "client request on socket fd %i\n", newsockfd);
        //}
        /* add request to queue for worker threads to handle */
        handler_enqueue_rqst(ctx);
        //gfs_handle_requests(ctx);

    }

}

void gfserver_stop(gfserver_t *gfs) {

    //clean up threads and clean up request queue
    gfs_cleanup();
}


/***********************/
/* gfcontext functions */
/***********************/

gfcontext_t* gfcontext_create(gfserver_t *gfs, struct sockaddr_in* cli_addr, socklen_t cli_addr_len, int sockfd) {
    gfcontext_t *ctx = malloc(sizeof(gfcontext_t));
    bzero(ctx, sizeof(*ctx));
    ctx->sockfd = sockfd;
    ctx->stat = GF_OK;
    ctx->cli_addr_len = cli_addr_len;
    ctx->cli_addr = cli_addr;
    ctx->gfs = gfs;
    return ctx;
}

void gfcontext_set_status(gfcontext_t* ctx, gfstatus_t stat) {
    ctx->stat = stat;
}

void gfcontext_cleanup(gfcontext_t* ctx) {
    //fprintf(stderr, "INFO gfcontext_cleanup called\n"); fflush(stderr);
    free(ctx);
    ctx = NULL;
}


/**************************/
/* private functions      */
/**************************/

void gfs_create_ok_header(char *hdr, size_t file_len) {
    sprintf(hdr, "%s %s %zu%s", scheme, stat_ok, file_len, mrkr);
}

void gfs_create_not_ok_header(char *hdr, gfstatus_t status) {
    if (status == GF_FILE_NOT_FOUND) {
        sprintf(hdr, "%s %s%s", scheme, stat_fnf, mrkr);
    }  else {
        sprintf(hdr, "%s %s%s", scheme, stat_er, mrkr);
    }
}

ssize_t gfs_sendheader(gfcontext_t *ctx, gfstatus_t status, size_t file_len) {

    char hdr[BUFSIZE];
    ssize_t bytes_sent;

    /* configure status info */
    switch (status) {
        case GF_OK:
            gfcontext_set_status(ctx, GF_OK);
            gfs_create_ok_header(hdr, file_len);
            break;
        case GF_FILE_NOT_FOUND:
            gfcontext_set_status(ctx, GF_FILE_NOT_FOUND);
            gfs_create_not_ok_header(hdr, status);
            break;
        default: /* including GF_ERROR */
            gfcontext_set_status(ctx, GF_ERROR);
            gfs_create_not_ok_header(hdr, status);
            break;
    }

    bytes_sent = send(ctx->sockfd, hdr, strlen(hdr), 0);  //don't send null terminator
    if (bytes_sent <= 0) {
        perror("[ERROR] sending header info to client\n");
        ctx->stat = GF_ERROR;
        return -1;
    } else {
        return bytes_sent;
    }

}

ssize_t gfs_send(gfcontext_t *ctx, void *data, size_t len) {

    ssize_t bytes_sent = send(ctx->sockfd, data, len, 0);  //don't send null terminator
    if (bytes_sent <= 0) {
        perror("[ERROR] sending info to client\n");
        ctx->stat = GF_ERROR;
        return -1;
    } else {
        return bytes_sent;
    }

}

void gfs_abort(gfcontext_t *ctx) {
//    fprintf(stderr, "INFO gfs_abort called\n"); fflush(stderr);
//    int n = close(ctx->sockfd);
//    if (n < 0)
//        fprintf(stderr, "ERROR closing the socket\n");
//    gfcontext_cleanup(ctx);
}

void gfs_init() {
    /* Initialize global resources, ex: request queue, mutexes, condition vars, shm_channel */

    int ret = 0;

    ret = pthread_mutex_init(&rqst_lock, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create mutex\n");
        exit(1);
    }

    ret = pthread_cond_init(&rqst_rdy, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create thread condition variable\n");
        pthread_mutex_destroy(&rqst_lock);
        exit(1);
    }

    /* setup request queue for worker threads */
    rqst_que = (steque_t *)malloc(sizeof(steque_t));
    bzero(rqst_que, sizeof(*rqst_que));
    steque_init(rqst_que);
}

void gfs_cleanup() {
    steque_destroy(rqst_que);
    free(rqst_que);
    pthread_mutex_destroy(&rqst_lock);
    pthread_cond_destroy(&rqst_rdy);
}


/****************************/
/* request queue management */
/****************************/
//TODO: don't need a queue item for a single pointer
typedef struct que_item {
    gfcontext_t *ctx;
} que_item;

que_item *create_que_item(gfcontext_t *ctx) {
    que_item *new_item = malloc(sizeof(que_item));
    bzero(new_item, sizeof(*new_item));
    new_item->ctx = ctx;
    return new_item;
}

void destroy_que_item(que_item *item) {
    free(item);
    item = NULL;
}

void handler_enqueue_rqst(gfcontext_t *ctx) {
    /* create package to add onto queue */
    usleep(100 * (random() % g_nthds));
    if (dbg) fprintf(stderr, "[INFO] Added request to queue\n");
    que_item *qi = create_que_item(ctx);
    pthread_mutex_lock(&rqst_lock);
    steque_enqueue(rqst_que, qi);
    pthread_cond_signal(&rqst_rdy);
    pthread_mutex_unlock(&rqst_lock);
}

void *handler_dequeue_rqsts(void *arg) {

    int tid = *((int*)(arg));
    if (dbg) fprintf(stderr, "[INFO] Thread %i is now handling request queue ...\n", tid);

    /* setup thread time out for waiting */
    struct timespec waittime;
    struct timeval now;
    int ret;

    while (1) {
        usleep(100 * (random() % g_nthds));

        pthread_mutex_lock(&rqst_lock);
        while (steque_isempty(rqst_que)) {

            /* set absolute time to wait until */
            gettimeofday(&now,NULL);
            waittime.tv_sec = now.tv_sec + 1;
            waittime.tv_nsec = 0;

            ret = pthread_cond_timedwait(&rqst_rdy, &rqst_lock, &waittime);
            if (ret == ETIMEDOUT) {
                if (dbg) fprintf(stderr, "[INFO] Thread %i waiting timed out waiting for queued requests\n", tid);
            }

        }
        if (dbg) fprintf(stderr, "[INFO] Thread %i dequeued request\n", tid);
        que_item *qi = (que_item *) steque_pop(rqst_que);
        pthread_cond_broadcast(&rqst_rdy);
        pthread_mutex_unlock(&rqst_lock);

        ssize_t byts_xfr = gfs_handle_requests(qi->ctx);
        if (byts_xfr != -1) {
            if (dbg) fprintf(stderr, "[INFO] Thread %i transferred %zu bytes\n", tid, (size_t) byts_xfr);
        } else {
            if (dbg) fprintf(stderr, "[ERROR] Thread %i encountered error in handle_file_request\n", tid);
        }
        destroy_que_item(qi);

    }
    return NULL; //never gets here but for completeness
}

int gfs_handle_requests(gfcontext_t *ctx) {

    size_t npos = 0;
    ssize_t bytes_recv;
    int got_hdr = 0;

    char buffer[BUFSIZE];
    bzero(buffer, BUFSIZE);
    char hdr_stuff[BUFSIZE];
    char *hdr_end = hdr_stuff;

    char filepath[512];
    bzero(filepath,sizeof(filepath));

    /* get next request and parse content */
    while((bytes_recv = recv(ctx->sockfd, buffer, BUFSIZE, 0)) > 0) {

        if (bytes_recv == -1) {
            fprintf(stderr, "[ERROR] connection terminated abnormally\n"); fflush(stderr);
            ctx->stat = GF_ERROR;
            break;
        }

        if (!got_hdr) {

            //fprintf(stderr, "INFO haven't got header, got byte count %zu\n", bytes_recv); fflush(stderr);

            if (bytes_recv == 0) {
                fprintf(stderr, "[ERROR] connection terminated normally but header was not fully received\n"); fflush(stderr);
                ctx->stat = GF_ERROR;
                break;
            }

            /* keep reading bytes and storing in header buffer until
             * marker is found.  Also any remaining bytes need to be stored b/c
             *  these may be the file contents */
            memcpy(hdr_end, buffer, (size_t)bytes_recv);
            hdr_end += bytes_recv;

            hdr_end = strstr(hdr_stuff, mrkr);
            if (hdr_end != NULL) { /* received full header, found marker */

                got_hdr = 1;

                /* check that first 7 chars provide scheme */
                if (memcmp( &hdr_stuff[0], scheme, strlen(scheme)) != 0) {
                    fprintf(stderr, "[ERROR] invalid scheme specified\n"); fflush(stderr);
                    ctx->stat = GF_FILE_NOT_FOUND;
                    break;
                }

                /* place buffer start location after scheme */
                npos = 1 + strlen(scheme); //1 accounts for whitespace or whatever delimiter, between scheme and status

                /* check for future but yet unsupported head method */
                if (memcmp( &hdr_stuff[npos], mthd_head, strlen(mthd_head)) == 0) {
                    fprintf(stderr, "[ERROR] head method not yet supported\n"); fflush(stderr);
                    ctx->stat = GF_FILE_NOT_FOUND;
                    break;
                }

                /* check that method is GET */
                if (memcmp( &hdr_stuff[npos], mthd_get, strlen(mthd_get)) != 0) {
                    fprintf(stderr, "[ERROR] method received from client is unknown\n"); fflush(stderr);
                    ctx->stat = GF_FILE_NOT_FOUND;
                    break;
                }

                /* get file path and check for validity */
                npos += (1+strlen(mthd_get)); //1 accounts for whitespace or whatever delimiter, between status and file path
                memcpy(filepath, &hdr_stuff[npos], (hdr_end-&hdr_stuff[npos]));
                /* check that file path starts with forward slash */
                if (strncmp(filepath, "/", 1) != 0) {
                    fprintf(stderr, "[ERROR] file path does not start with forward slash (/) : %s\n", filepath); fflush(stderr);
                    ctx->stat = GF_FILE_NOT_FOUND;
                }
                break;

            }
        }
    }

    int stat;
    if (ctx->stat == GF_OK) {

        /* call handler for responding to request */
        if (dbg) fprintf(stderr, "[INFO] request is %s\n", hdr_stuff);
        ssize_t n = ctx->gfs->hndlr_func(ctx, filepath, ctx->gfs->hndlr_arg);
        if (n < 0) {
            fprintf(stderr, "[ERROR] in handler when responding to request\n");
            stat = -1;
        } else { // all is well or handler took care of error handling
            stat = 0;
        }
    } else {
        /* report error back to client */
        gfs_sendheader(ctx, ctx->stat, 0);
        stat = -1;
    }

    close(ctx->sockfd);
    gfcontext_cleanup(ctx);
    return stat;

}