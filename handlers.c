#include <stdlib.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <asm/errno.h>

#include "gfserver.h"
#include "shm_channel.h"
#include "steque.h"

static int dbg = 1;

static ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg);
static ssize_t handle_with_curl(gfcontext_t *ctx, char *path, void* arg);

/*********************************/
/* request handler main function */
/*********************************/
extern ssize_t handle_request(gfcontext_t *ctx, char *path, void* arg) {

    ssize_t byts_xfrd = 0;

    /* first try to get file from cache */
    ctx->stat = GF_ERROR; //start with assuming error, handler has to change to OK
    byts_xfrd = handle_with_cache(ctx, path, arg);
    if (byts_xfrd < 0) {

        if (dbg) fprintf(stderr, "[ERROR] file not found when attempting to transfer file using cahce.  Trying http server.\n");

        /* if file not found or error occurred when requesting from cache,
         *  try to get file from server using curl
         */
        byts_xfrd = 0;
        ctx->stat = GF_ERROR; //start with assuming error, handler has to change to OK
        byts_xfrd = handle_with_curl(ctx, path, arg);
        if (byts_xfrd < 0) {
            switch(ctx->stat) {
                case GF_FILE_NOT_FOUND:
                    if (dbg) fprintf(stderr, "[ERROR] file not found when attempting to transfer file using curl.\n");
                    gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
                    break;
                default:
                    if (dbg) fprintf(stderr, "[ERROR] unknown error occurred when attempting to transfer file curl.\n");
                    gfs_sendheader(ctx, GF_ERROR, 0);
                    break;
            }
        }

    } else {

    }

    return byts_xfrd;

}


/**************************************/
/* cache file transfer specific stuff */
/**************************************/

extern steque_t *mem_seg_que;
extern pthread_mutex_t mem_seg_lock;
extern pthread_cond_t mem_seg_rdy;

void handler_enq_mem_seg(int *mem_seg_id);
int *handler_deq_mem_seg();

ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg) {

    //char *server = (char *)arg;  //server url
    int ret = 0;

    /* attempt to claim a free memory segment */
    int *mem_seg_id = handler_deq_mem_seg();
    void *mem_addr = shm_attach_mem_seg(*mem_seg_id);
    if (mem_addr == NULL) {
        //unknown error occurred when trying to send file
        fprintf(stderr, "[ERROR] cache client - could not attach shared memory segment w id: %d.\n", *mem_seg_id);
        //gfs_sendheader(ctx, GF_ERROR, 0);
        ctx->stat  = GF_ERROR;
        handler_enq_mem_seg(mem_seg_id); //make message segment available for others
        return -1;
    }

    /* submit request for file to cache daemon */
    shm_context_t *shm_ctx = shm_context_create(path, *mem_seg_id);
    ret = shm_client_send_file_request(shm_ctx);
    if (ret == -1) {
        //unknown error occurred when trying to send file
        fprintf(stderr, "[ERROR] cache client - unknown error occurred when trying to send file request, err code: %d\n", shm_context_get_error(shm_ctx));
        //gfs_sendheader(ctx, GF_ERROR, 0);
        ctx->stat  = GF_ERROR;
        ret = -1;
    } else if (shm_context_get_error(shm_ctx) == 404) {
        /* file doesn't exist in cache, send file not found */
        fprintf(stderr, "[ERROR] cache returned 404 (file not found) error.\n");
        //gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
        ctx->stat  = GF_FILE_NOT_FOUND;
        ret = -1;
    } else {

        /* all is well send file length information via gf protocol */
        size_t file_len = shm_context_get_file_size(shm_ctx);
        gfs_sendheader(ctx, GF_OK, file_len);

        /* send the data */
        size_t mem_seg_sz = shm_context_get_seg_tot_sz(shm_ctx);
        char buffer[mem_seg_sz];
        ssize_t bytes_transferred = 0;
        ssize_t read_len, write_len;
        while (bytes_transferred < file_len) {

            shm_client_wait_for_ready(shm_ctx);

            /* read from shared mem */
            read_len = shm_read_mem_seg(mem_addr, buffer, shm_context_get_seg_used_sz(shm_ctx));
            if (read_len <= 0){
                fprintf(stderr, "[ERROR] client - handle_with_cache mem seg read error, %zd, %zu, %zu", read_len, bytes_transferred, file_len );
                return -1;
                //TODO: send error and resend request to server
            }

            /* read was successful, acknowledge server */
            shm_client_send_acknowledge(shm_ctx);

            /* send contents via gf protocol */
            write_len = gfs_send(ctx, buffer, (size_t)read_len);
            if (write_len != read_len){
                fprintf(stderr, "[ERROR] cache client - handle_with_cache gf_send error");
                return -1;
            }
            bytes_transferred += write_len;
        }

        ret = bytes_transferred;
        if (dbg) fprintf(stderr, "[INFO] cahce client - success transferring file!!!\n");
    }

    /* clean up */
    shm_detach_mem_seg(mem_addr);
    handler_enq_mem_seg(mem_seg_id); //make message segment available for others
    shm_context_cleanup(shm_ctx);

    return ret;

}


/* shared memory queue handlers */

void handler_enq_mem_seg(int *mem_seg_id) {
    usleep(100 * (random() % 10));
    pthread_mutex_lock(&mem_seg_lock);
    steque_enqueue(mem_seg_que, mem_seg_id);
    pthread_cond_signal(&mem_seg_rdy);
    pthread_mutex_unlock(&mem_seg_lock);
    if (dbg) fprintf(stderr, "[INFO] Added mem segment back onto queue: %d\n", *mem_seg_id);
}

int *handler_deq_mem_seg() {
    usleep(100 * (random() % 10));
    pthread_mutex_lock(&mem_seg_lock);
    while (steque_isempty(mem_seg_que)) {
        pthread_cond_wait(&mem_seg_rdy, &mem_seg_lock);
    }
    int *mem_seg_id = (int *)steque_pop(mem_seg_que);
    pthread_cond_broadcast(&mem_seg_rdy);
    pthread_mutex_unlock(&mem_seg_lock);
    if (dbg) fprintf(stderr, "[INFO] Removed mem seg from queue: %d\n", *mem_seg_id);
    return mem_seg_id;
}


/******************************************/
/* http/curl file transfer specific stuff */
/******************************************/

typedef struct curl_data {
    gfcontext_t *ctx;
    size_t tot_bytes_sent;
    int err_stat;
} curl_data;

size_t curl_hdr_cb(char *buffer, size_t size, size_t nmemb, void *userdata) {

    char *lclbuffer = malloc(nmemb + 1);
    bzero(lclbuffer, nmemb + 1);
    strcpy(lclbuffer, buffer);

    size_t recv_size = size*nmemb;
    size_t ret_stat = recv_size;

    curl_data *cd = (curl_data *) userdata;

    //parse the next incoming header line and look for content length string
    char *src_cnt_lng = "content-length:";
    char *src_fbd = "403";
    char *src_nfd = "404";
    char *src_ok = "200";
    char *lngth;
    if (strncasecmp(lclbuffer, src_cnt_lng, 15) == 0) {
        //found content length string
        lngth = strtok(lclbuffer, ":");
        lngth = strtok(NULL, ":");
        if (lngth == NULL) {
            if (dbg) fprintf(stderr, "ERROR: found content-length string but no size.\n");
            cd->err_stat = 1;
            return 0;
        }
        //fprintf(stderr, "INFO: Thread %s found content length!!!\n", thd_id);
        cd->err_stat = 0;
        gfs_sendheader(cd->ctx, GF_OK, (size_t) atol(lngth));
    } else if (strstr(lclbuffer, src_ok) != 0) {
        if (dbg) fprintf(stderr, "INFO: Server replied with OK.\n");
        cd->err_stat = 200;
    } else if (strstr(lclbuffer, src_fbd) != 0) {
        if (dbg) fprintf(stderr, "ERROR: Server replied with forbidden response.\n");
        cd->err_stat = 403;
        ret_stat = 0;
    } else if (strstr(lclbuffer, src_nfd) != 0) {
        if (dbg) fprintf(stderr, "ERROR: Server replied with not found response.\n");
        cd->err_stat = 404;
        ret_stat = 0;
    }
    free(lclbuffer);
    return ret_stat;
}

size_t curl_writ_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {

    size_t bytes_transferred = 0;
    size_t recv_size = size*nmemb;

    curl_data *cd = (curl_data *)userdata;
    /* check that file length was extracted from header */
    if (cd->err_stat != 0) {
        if (dbg) fprintf(stderr, "[ERROR] header not properly parsed before receiving data.  Shouldn't have gotten here.\n");
        return 0;
    }

    /* Sending the data contents chunk by chunk. */
    bytes_transferred = 0;
    ssize_t write_len;
    while(bytes_transferred < recv_size){
        write_len = gfs_send(cd->ctx, ptr, recv_size);
        if (write_len != recv_size){
            fprintf(stderr, "[ERROR] unable to send enter data fragment via gf_send.\n");
            cd->err_stat = 1;
            return 0;
        }
        cd->tot_bytes_sent += write_len;
        bytes_transferred += write_len;
    }

    return bytes_transferred;

}

int perf_curl(CURL *curl, char *url) {
    CURLcode res;
    long curl_res_code;
    /* set URL to send request */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (dbg) fprintf(stderr, "[INFO] Request is: %s\n", url);
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &curl_res_code);
    if(CURLE_OK != res) {
        if (dbg) fprintf(stderr, "[ERROR] encountered libcurl specific error: %d\n", res);
        if (dbg) fprintf(stderr, "       server response code is: %ld\n", curl_res_code);
    }
    return res;
}

void cleanup_curl(CURL *curl) {
    curl_easy_cleanup(curl);
    curl_global_cleanup();
}

ssize_t handle_with_curl(gfcontext_t *ctx, char *path, void* arg) {

    char path_src[4096];
    char *data_src = arg;
    bzero(path_src, sizeof(path_src));
    strcpy(path_src,data_src);
    strcat(path_src,path);

    CURL *curl;

    /* create data structure for curl information */
    curl_data cd;
    cd.ctx = ctx;
    cd.tot_bytes_sent = 0;
    cd.err_stat = 1; //has to be set to zero from header function

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {

        /* Switch on full protocol/debug output */
        //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        /* callback and custom args */
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_hdr_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cd);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writ_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cd);

    } else {
        gfs_sendheader(ctx, GF_ERROR, 0);
        return EXIT_FAILURE;
    }

    int res = perf_curl(curl, path_src);
    cleanup_curl(curl);
    if (res !=0 && cd.err_stat == 404) {
        /* If 404 received from proxy, then send FILE_NOT_FOUND code */
        fprintf(stderr, "[ERROR] http returned 404 error\n");
        gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
        return EXIT_FAILURE;
    } else if (res !=0) {
        fprintf(stderr, "[ERROR] http returned other error, code: %d\n", cd.err_stat);
        gfs_sendheader(ctx, GF_ERROR, 0);
        return EXIT_FAILURE;
    }

    return cd.tot_bytes_sent;

}
