#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <asm/errno.h>

#include "shm_channel.h"
#include "simplecache.h"
#include "steque.h"

#define MAX_CACHE_REQUEST_LEN 256

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  simplecached [options]\n"                                                  \
"options:\n"                                                                  \
"  -t [thread_count]   Num worker threads (Default: 1, Range: 1-1000)\n"      \
"  -c [cachedir]       Path to static files (Default: ./)\n"                  \
"  -h                  Show this help message\n"

static int dbg = 0;

static steque_t *rqst_que;
static pthread_mutex_t rqst_lock;
static pthread_cond_t rqst_rdy;
static int g_nthds = 0;

/* forward declarations */
static void _init_stuff();
static void _cleanup_stuff();
static void _sig_handler(int signo);
static void handler_enqueue_rqst(shm_context_t *ctx);
static void *handler_dequeue_rqsts(void *);
static ssize_t handle_file_request(shm_context_t *ctx);

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
    {"nthreads",           required_argument,      NULL,           't'},
    {"cachedir",           required_argument,      NULL,           'c'},
    {"help",               no_argument,            NULL,           'h'},
    {NULL,                 0,                      NULL,             0}
};

void Usage() {
    fprintf(stdout, "%s", USAGE);
}

int main(int argc, char **argv) {
    int nthreads = 1;
    char *cachedir = "locals.txt";
    char option_char;


    while ((option_char = getopt_long(argc, argv, "t:c:h", gLongOptions, NULL)) != -1) {
        switch (option_char) {
            case 't': // thread-count
                nthreads = atoi(optarg);
                break;
            case 'c': //cache directory
                cachedir = optarg;
                break;
            case 'h': // help
                Usage();
                exit(0);
                break;
            default:
                Usage();
                exit(1);
        }
    }

    if (signal(SIGINT, _sig_handler) == SIG_ERR){
        fprintf(stderr,"[Error] Can't catch SIGINT...exiting.\n");
        exit(-1);
    }

    if (signal(SIGTERM, _sig_handler) == SIG_ERR){
        fprintf(stderr,"[Error] Can't catch SIGTERM...exiting.\n");
        exit(-1);
    }

    if ((nthreads < 1) || (nthreads>1024)) {
        nthreads = 1;
    }
    g_nthds = nthreads;

    //fprintf(stderr, "[INFO] cache started\n");

    /* initialize stuff */
    _init_stuff();

    /* Initializing the cache */
    simplecache_init(cachedir);

    /* start worker threads */
    if (dbg) fprintf(stderr, "[INFO] creating threads ... \n");
    pthread_t thrds[nthreads];
    for (int ithd = 0; ithd < nthreads; ithd++) {
        int rc = pthread_create(&thrds[ithd], NULL, handler_dequeue_rqsts, (void*)&ithd);
        if (rc) {
            fprintf(stderr, "[ERROR] when attempting to create thread %i\n - %d", ithd, rc);
            return 1;
        }
    }

    /* start handling requests via message queue*/
    if (dbg) fprintf(stderr, "[INFO] server ready for requests\n");
    while (1) {

        /* wait for new request */
        shm_context_t *rqst_ctx = shm_server_wait_for_file_request();
        if (rqst_ctx == NULL) {
            fprintf(stderr, "[ERROR] something went wrong when waiting for message queue requests.\n");
        }

        handler_enqueue_rqst(rqst_ctx);
    }

}

void _init_stuff() {
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

    /* create message queue for file transfer communication */
    if (shm_init_msg_que() != 0) {
        fprintf(stderr, "[ERROR] cannot create IPC message queue.\n");
        exit(1);
    }

}

void _cleanup_stuff() {

    simplecache_destroy();

    /* check for any messages on the queue and if so need
     * to reply with shutting down message before
     * destroying, BUT prj instructions say something about
     * no need to check for mis-behaving clients */
    if (shm_destroy_msg_que() != 0) {
        fprintf(stderr, "[ERROR] cannot destroy IPC message queue.\n");
        exit(1);
    }

    steque_destroy(rqst_que);
    free(rqst_que);

    pthread_mutex_destroy(&rqst_lock);
    pthread_cond_destroy(&rqst_rdy);
}

void _sig_handler(int signo){
    if (signo == SIGINT || signo == SIGTERM){
        fprintf(stderr, "[WARN] webrpoxy received SIGINT or SIGTERM, cleaning up and closing\n");
        _cleanup_stuff();
        exit(signo);
    }
}


/**************************************
 *shm_channel message queue management
 *************************************/
//TODO: don't need a queue item for a single pointer
typedef struct que_item {
    shm_context_t *ctx;
} que_item;

que_item *create_que_item(shm_context_t *ctx) {
    que_item *new_item = malloc(sizeof(que_item));
    bzero(new_item, sizeof(*new_item));
    new_item->ctx = ctx;
    return new_item;
}

void destroy_que_item(que_item *item) {
    free(item);
    item = NULL;
}

void handler_enqueue_rqst(shm_context_t *ctx) {
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

        ssize_t byts_xfr = handle_file_request(qi->ctx);
        if (byts_xfr != -1) {
            if (dbg) fprintf(stderr, "[INFO] Thread %i transferred %zu bytes\n", tid, (size_t) byts_xfr);
        } else {
            if (dbg) fprintf(stderr, "[ERROR] Thread %i encountered error in handle_file_request\n", tid);
        }
        destroy_que_item(qi);

    }
    return NULL; //never gets here but for completeness
}

ssize_t handle_file_request(shm_context_t *ctx) {
    int fildes= 0;
    int ret = 0;
    ssize_t file_len, bytes_transferred;
    ssize_t read_len, write_len;
    char buffer[shm_context_get_seg_tot_sz(ctx)];

    if ( 0 > (fildes = simplecache_get(shm_context_get_file_path(ctx)))) {
        /* file not found */
        fprintf(stderr, "[ERROR] server - could not get file descriptor from simplecache for file: %s\n", shm_context_get_file_path(ctx));
        shm_context_set_error(ctx, SHM_STAT_NOT_FOUND);
        shm_server_send_response(ctx);
        shm_context_cleanup(ctx);
        return -1;
    }

    /* calculating file size and send ok */
    file_len = lseek(fildes, 0, SEEK_END);
    lseek(fildes, 0, SEEK_SET);
    shm_context_set_error(ctx, SHM_STAT_OK);
    shm_context_set_file_size(ctx, (size_t)file_len);
    ret = shm_server_send_response(ctx);
    if (ret == -1) {
        fprintf(stderr, "[ERROR] server - could not send ok response back to client\n");
        shm_context_cleanup(ctx);
        return -1;
    }

    /* attach to shared memory segment */
    void *shm_addr = shm_attach_mem_seg(shm_context_get_seg_id(ctx));
    if (shm_addr == NULL) {
        fprintf(stderr, "[ERROR] server - shared memory address should have existed but was not found.  Client may have closed.\n");
        shm_context_cleanup(ctx);
        return -1;
    }

    int ret_val = 0;

    /* sending the file contents chunk by chunk. */
    bytes_transferred = 0;
    while (bytes_transferred < file_len) {

        read_len = pread(fildes, buffer, shm_context_get_seg_tot_sz(ctx), bytes_transferred);
        if (read_len <= 0){
            fprintf(stderr, "[ERROR] file read error, %zd, %zu, %zu", read_len, bytes_transferred, file_len );
            ret_val = -1;
            break;
        }

        write_len = shm_write_mem_seg(shm_addr, buffer, (size_t)read_len);
        if (write_len != read_len){
            fprintf(stderr, "[ERROR] handle_with_file write error");
            ret_val = -1;
            break;
        }

        shm_context_set_seg_used_sz(ctx, (size_t)write_len);
        shm_server_send_ready(ctx);
        shm_server_wait_for_acknowledge(ctx);

        bytes_transferred += write_len;
    }

    if (ret_val != -1) {
        ret_val = bytes_transferred;
    }
    shm_detach_mem_seg(shm_addr);
    shm_context_cleanup(ctx);

    return ret_val;
}