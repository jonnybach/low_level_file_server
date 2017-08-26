#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "workload.h"
#include "gfclient.h"
#include "steque.h"

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  webclient [options]\n"                                                     \
"options:\n"                                                                  \
"  -s [server_addr]    Server address (Default: 0.0.0.0)\n"                   \
"  -p [server_port]    Server port (Default: 8888)\n"                         \
"  -w [workload_path]  Path to workload file (Default: workload.txt)\n"       \
"  -t [nthreads]       Number of threads (Default 1)\n"                       \
"  -n [num_requests]   Requests download per thread (Default: 1)\n"           \
"  -h                  Show this help message\n"                              \

static const int dbg = 1;

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
        {"server",        required_argument,      NULL,           's'},
        {"port",          required_argument,      NULL,           'p'},
        {"workload-path", required_argument,      NULL,           'w'},
        {"nthreads",      required_argument,      NULL,           't'},
        {"nrequests",     required_argument,      NULL,           'n'},
        {"help",          no_argument,            NULL,           'h'},
        {NULL,            0,                      NULL,             0}
};

/* global declarations */
static steque_t *rqst_que;
static pthread_mutex_t rqst_lock;
static pthread_cond_t rqst_rdy;
static int rqst_cnt = 0;
static int g_nthds = 0;

typedef struct que_item {
    char *server;
    unsigned short port;
    char *filepath;
    void* arg;
    int id;
} que_item;

/* forward declarations */
static que_item *create_que_item(char *server, unsigned short port, char *filepath, int qid, void *arg);
static void destroy_que_item(que_item *item);
static void enqueue_rqst(char *server, unsigned short port, char *filepath, int qid, void* arg);
static void *dequeue_rqsts(void *arg);
static size_t perform_xfer(char *server, unsigned short port, char *req_path);
static void _init_global_def();
static void _clean_global_def();

static void Usage() {
    fprintf(stdout, "%s", USAGE);
}

static void localPath(char *req_path, char *local_path){
    static int counter = 0;

    sprintf(local_path, "%s-%06d", &req_path[1], counter++);
}

static FILE* openFile(char *path){
    char *cur, *prev;
    FILE *ans;

    /* Make the directory if it isn't there */
    prev = path;
    while(NULL != (cur = strchr(prev+1, '/'))){
        *cur = '\0';

        if (0 > mkdir(&path[0], S_IRWXU)){
            if (errno != EEXIST){
                perror("Unable to create directory");
                exit(1);
            }
        }

        *cur = '/';
        prev = cur;
    }

    if( NULL == (ans = fopen(&path[0], "w"))){
        perror("Unable to open file");
        exit(1);
    }

    return ans;
}

/* Callbacks */
static void writecb(void* data, size_t data_len, void *arg){
    FILE *file = (FILE*) arg;
    fwrite(data, 1, data_len, file);
}

/* Main */
int main(int argc, char **argv) {
/* COMMAND LINE OPTIONS */
    char *server = "localhost";
    unsigned short port = 8888;
    char *workload_path = "workload.txt";

    int option_char = 0;
    int nrequests = 1;
    int nthreads = 1;

    // Parse and set command line arguments
    while ((option_char = getopt_long(argc, argv, "s:p:w:n:t:h", gLongOptions, NULL)) != -1) {
        switch (option_char) {
            case 's': // server
                server = optarg;
                break;
            case 'p': // port
                port = atoi(optarg);
                break;
            case 'w': // workload-path
                workload_path = optarg;
                break;
            case 'n': // nrequests
                nrequests = atoi(optarg);
                break;
            case 't': // nthreads
                nthreads = atoi(optarg);
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

    if( 0 != workload_init(workload_path)){
        fprintf(stderr, "Unable to load workload file %s.\n", workload_path);
        exit(1);
    }

    if (dbg) fprintf(stderr, "[INFO] Threads requested: %i\n", nthreads);
    if (nthreads < 1) {
        nthreads = 1;
    }
    g_nthds = nthreads;

    _init_global_def();
    gfc_global_init();
    rqst_cnt = nrequests * nthreads;

    /* start worker threads */
    if (dbg) fprintf(stderr, "[INFO] creating threads ... \n");
    pthread_t thrds[nthreads];
    for (int ithd = 0; ithd < nthreads; ithd++) {
        if (dbg) fprintf(stderr, "[INFO] creating thread %i ... \n", ithd);
        int rc = pthread_create(&thrds[ithd], NULL, dequeue_rqsts, &ithd);
        if (rc) {
            fprintf(stderr, "ERROR when attempting to create thread %i\n - %d", ithd, rc);
            return 1;
        }
    }

    /* perform requests */
    char *req_path;
    for(int i = 0; i < nrequests * nthreads; i++){
        req_path = workload_get_path();
        enqueue_rqst(server, port, req_path, i, NULL);
    }

    for (int ithd = 0; ithd < nthreads; ithd++) {
        int rc = pthread_join(thrds[ithd], NULL);
        if (rc) {
            fprintf(stderr, "ERROR when trying to join thread %i\n - %d", ithd, rc);
            return 1;
        }
    }

    if (dbg) fprintf(stderr, "INFO all threads complete\n");
    steque_destroy(rqst_que);
    gfc_global_cleanup();
    _clean_global_def();

    return 0;
}

static que_item *create_que_item(char *server, unsigned short port, char *filepath, int qid, void *arg) {
    que_item *new_item = malloc(sizeof(que_item));
    bzero(new_item, sizeof(&new_item));
    new_item->server = strdup(server);
    new_item->port = port;
    new_item->filepath = strdup(filepath);
    new_item->arg = arg;
    new_item->id = qid;
    return new_item;
}

static void destroy_que_item(que_item *item) {
    free(item->server);
    free(item->filepath);
    free(item);
    item = NULL;
}

static void enqueue_rqst(char *server, unsigned short port, char *filepath, int qid, void* arg) {
    /* create package to add onto queue */
    usleep(100 * (random() % g_nthds));
    fprintf(stderr, "[INFO] Adding request %i to queue, filpath: %s\n", qid, filepath); fflush(stderr);
    que_item *qi = create_que_item(server, port, filepath, qid, arg);
    pthread_mutex_lock(&rqst_lock);
    steque_enqueue(rqst_que, qi);
    pthread_cond_signal(&rqst_rdy);
    pthread_mutex_unlock(&rqst_lock);
}

static void *dequeue_rqsts(void *arg) {

    int tid = *((int*)(arg));
    if (dbg) fprintf(stderr, "[INFO] Thread %i is now handling request queue ...\n", tid);

    /* setup thread time out for waiting */
    struct timespec waittime;
    struct timeval now;
    int ret;

    que_item *qi = NULL;
    int done = 0;

    while (!done) {
        usleep(100 * (random() % g_nthds));

        pthread_mutex_lock(&rqst_lock);
        while (steque_isempty(rqst_que) && rqst_cnt != 0) {

            /* set absolute time to wait until */
            gettimeofday(&now,NULL);
            waittime.tv_sec = now.tv_sec + 1;
            waittime.tv_nsec = 0;
            ret = pthread_cond_timedwait(&rqst_rdy, &rqst_lock, &waittime);
            if (ret == ETIMEDOUT) {
                fprintf(stderr, "[WARN] Thread %i waiting timed out, this shouldn't occur\n", tid);
            }
        }

        if (steque_isempty(rqst_que)) {
            done = 1;
        } else {
            qi = (que_item *) steque_pop(rqst_que);
            rqst_cnt --;
        }
        pthread_cond_broadcast(&rqst_rdy);
        pthread_mutex_unlock(&rqst_lock);

        if (!done) {
            fprintf(stderr, "[INFO] Thread %i handling request id %i, filepath: %s\n", tid, qi->id, qi->filepath);
            fflush(stderr);
            ssize_t byts_xfr = perform_xfer(qi->server, qi->port, qi->filepath);
            destroy_que_item(qi);
            fprintf(stderr, "[INFO] Number of requests is now: %i\n", rqst_cnt);
            fflush(stderr);
            fprintf(stderr, "[INFO] Thread %i transferred %zu bytes\n", (int) tid, (size_t) byts_xfr);
            fflush(stderr);
        }

    }

    return NULL;

}

static size_t perform_xfer(char *server, unsigned short port, char *req_path) {

    int returncode;

    FILE *file;
    char local_path[512];

    if(strlen(req_path) > 256){
        fprintf(stderr, "[ERROR] Request path exceeded maximum of 256 characters\n.");
        exit(1);
    }

    localPath(req_path, local_path);

    file = openFile(local_path);

    gfcrequest_t *gfr;
    gfr = gfc_create();
    gfc_set_server(gfr, server);
    gfc_set_path(gfr, req_path);
    gfc_set_port(gfr, port);
    gfc_set_writefunc(gfr, writecb);
    gfc_set_writearg(gfr, file);

    if (dbg) fprintf(stderr, "[INFO] Requesting %s%s\n", server, req_path);

    if ( 0 > (returncode = gfc_perform(gfr))) {
        fprintf(stderr, "[ERROR] gfc_perform returned an error %d\n", returncode);
        fclose(file);
        if (0 > unlink(local_path)) {
            fprintf(stderr, "[ERROR] unlink failed on %s\n", local_path);
        }
    } else {
        fclose(file);
    }

    if ( gfc_get_status(gfr) != GF_OK){
        if ( 0 > unlink(local_path))
            fprintf(stderr, "[ERROR] unlink failed on %s\n", local_path);
    }

    size_t byts_rcv = gfc_get_bytesreceived(gfr);
    size_t file_len = gfc_get_filelen(gfr);
    char *stat = strdup( gfc_strstatus(gfc_get_status(gfr)));

    gfc_cleanup(gfr);

    if(dbg) {
        fprintf(stdout, "[INFO] gfclient - status: %s\n", stat);
        fprintf(stdout, "[INFO] gfclient - received %zu of %zu bytes\n", byts_rcv, file_len);
    }
    free(stat);
    return byts_rcv;
}

static void _init_global_def() {
    /* Initialize global resources, ex: request queue, mutexes, condition vars */
    rqst_que = (steque_t *)malloc(sizeof(steque_t));
    bzero(rqst_que, sizeof(&rqst_que));
    steque_init(rqst_que);
    int ret = 0;
    ret = pthread_mutex_init(&rqst_lock, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create mutex\n");
        exit(1);
    }
    ret = pthread_cond_init(&rqst_rdy, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create add thread condition variable\n");
        exit(1);
    }
    ret = pthread_cond_init(&rqst_rdy, NULL);
    if (ret) {
        fprintf(stderr, "[ERROR] when attempting to create thread condition variable\n");
        exit(1);
    }
}

static void _clean_global_def() {
    /* free and clean up global resources, ex: request queue, mutexes, condition vars */
    steque_destroy(rqst_que);
    free(rqst_que);
    pthread_mutex_destroy(&rqst_lock);
    pthread_cond_destroy(&rqst_rdy);
}