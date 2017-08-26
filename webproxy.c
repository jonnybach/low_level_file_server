#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory.h>

#include "gfserver.h"
#include "shm_channel.h"


#define USAGE                                                                 \
"usage:\n"                                                                    \
"  webproxy [options]\n"                                                      \
"options:\n"                                                                  \
"  -n [seg count]      Number of segments to use in communication with cache (Default: 1).\n"\
"  -z [seg size]       The size (in bytes) of the segments (Default: 1024).\n"\
"  -p [listen_port]    Listen port (Default: 8888)\n"                         \
"  -t [thread_count]   Num worker threads (Default: 1, Range: 1-1000)\n"      \
"  -s [server]         The server to connect to (Default: Udacity S3 instance)\n"\
"  -h                  Show this help message\n"                              \
"special options:\n"                                                          \
"  -d [drop_factor]    Drop connects if f*t pending requests (Default: 5).\n"


/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
        {"seg_count",     required_argument,      NULL,           'n'},
        {"seg_size",      required_argument,      NULL,           'z'},
        {"port",          required_argument,      NULL,           'p'},
        {"thread-count",  required_argument,      NULL,           't'},
        {"server",        required_argument,      NULL,           's'},
        {"help",          no_argument,            NULL,           'h'},
        {NULL,            0,                      NULL,             0}
};

/* extern and global declarations */

extern ssize_t handle_request(gfcontext_t *ctx, char *path, void* arg);
extern void handler_enq_mem_seg(int *mem_seg_id);
extern steque_t *mem_seg_que;

steque_t *mem_seg_que;
pthread_mutex_t mem_seg_lock;
pthread_cond_t mem_seg_rdy;

static gfserver_t gfs;
static int *_mem_seg_ids;
static size_t _n_mem_segs;

/* forward declarations */
static void _init_stuff(unsigned short num_segs, size_t seg_size);
static void _cleanup_stuff();
static void _sig_handler(int signo);


void Usage() {
    fprintf(stdout, "%s", USAGE);
}

int main(int argc, char **argv) {
    int option_char = 0;
    unsigned short seg_count = 1;
    size_t seg_size = 1024;
    unsigned short port = 8888;
    unsigned short nworkerthreads = 1;
    char *server = "s3.amazonaws.com/content.udacity-data.com";

    /* Parse and set command line arguments */
    while ((option_char = getopt_long(argc, argv, "n:z:p:t:s:h", gLongOptions, NULL)) != -1) {
        switch (option_char) {
            case 'n': // listen-port
                seg_count = atoi(optarg);
                break;
            case 'z': // listen-port
                seg_size = atoi(optarg);
                break;
            case 'p': // listen-port
                port = atoi(optarg);
                break;
            case 't': // thread-count
                nworkerthreads = atoi(optarg);
                break;
            case 's': // server address
                server = optarg;
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

    if (seg_size == 0) {
        fprintf(stderr, "[Error] Memory segment size cannot be zero.\n");
        exit(1);
    }

    if (signal(SIGINT, _sig_handler) == SIG_ERR) {
        fprintf(stderr, "[Error] Can't catch SIGINT...exiting.\n");
        exit(1);
    }

    if (signal(SIGTERM, _sig_handler) == SIG_ERR) {
        fprintf(stderr, "[Error] Can't catch SIGTERM...exiting.\n");
        exit(1);
    }

    if (!server) {
        fprintf(stderr, "[Error] Invalid (null) server name\n");
        exit(1);
    }

    //fprintf(stderr, "[INFO] proxy started\n");

    _init_stuff(seg_count, seg_size);

    /* initializing server */
    gfserver_init(&gfs, nworkerthreads);

    /* setting options */
    gfserver_set_port(&gfs, port);
    gfserver_set_maxpending(&gfs, 10);

    /* set handler callback and custom argument */
    gfserver_set_handler(&gfs, handle_request);
    gfserver_set_handlerarg(&gfs, server);

    /* loops forever */
    gfserver_serve(&gfs);
}

void _init_stuff(unsigned short num_segs, size_t seg_size) {
    /* Initialize global resources, ex: request queue, mutexes, condition vars,
     *  shm_channel */

    int ret = 0;

    ret = pthread_mutex_init(&mem_seg_lock, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create mutex\n");
        exit(1);
    }

    ret = pthread_cond_init(&mem_seg_rdy, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create thread condition variable\n");
        pthread_mutex_destroy(&mem_seg_lock);
        exit(1);
    }

    /* init shared memory segments */
    if (shm_init_mem_segs(num_segs, seg_size) != 0) {
        fprintf(stderr, "[ERROR] problem creating IPC shared memory segments.\n");
        pthread_mutex_destroy(&mem_seg_lock);
        pthread_cond_destroy(&mem_seg_rdy);
        exit(1);
    }

    /* setup memory segment queue to control mem seg usage by threads */
    mem_seg_que = (steque_t *)malloc(sizeof(steque_t));
    bzero(mem_seg_que, sizeof(*mem_seg_que));
    steque_init(mem_seg_que);

    /* add shm initialized memory segments into
     * webproxy's mem seg queue */
    _mem_seg_ids = shm_get_mem_seg_ids(&_n_mem_segs);
    steque_init(mem_seg_que);
    for (int i = 0; i < _n_mem_segs; i++) {
        handler_enq_mem_seg((_mem_seg_ids+i));
    }

    /* connect to the message queue */
    if (shm_connect_to_msg_que() != 0) {
        fprintf(stderr, "[ERROR] cannot create IPC message queue.\n");
        _cleanup_stuff();
        exit(1);
    }

}

void _cleanup_stuff() {

    if (shm_destroy_mem_segs() != 0) {
        fprintf(stderr, "[ERROR] cannot destroy shared memory segments.\n");
        exit(1);
    }

    steque_destroy(mem_seg_que);
    free(_mem_seg_ids);
    _n_mem_segs = 0;

    pthread_cond_destroy(&mem_seg_rdy);
    pthread_mutex_destroy(&mem_seg_lock);

}

void _sig_handler(int signo){
    if (signo == SIGINT || signo == SIGTERM){
        fprintf(stderr, "[WARN] webrpoxy received SIGINT or SIGTERM, cleaning up and closing\n");
        gfserver_stop(&gfs);
        _cleanup_stuff();
        exit(signo);
    }
}
