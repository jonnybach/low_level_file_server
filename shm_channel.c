//In case you want to implement the shared memory IPC as a library...
#include <sys/msg.h>
#include <sys/shm.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "shm_channel.h"

#define SHM_MAIN_CHAN_C 1
#define SHM_MAIN_CHAN_S 2
#define SHM_MAIN_CHAN_OFST 2

#define WAIT_TIME_SEC 2
#define WAIT_TRYS 25

#define SHM_MSG_WAIT 0
#define SHM_MSG_NOWAIT 1

static const int dbg = 0;
static const int dbg_mque = 0;


/************************/
/* memory segment stuff */
key_t _mem_key_seed = (key_t)514332585485642;
unsigned short _num_mem_segs;
size_t _mem_seg_sz;
typedef struct _mem_seg {
    int seg_id;
//    size_t seg_sz;
//    void *addr;
} shm_mem_seg_t;
shm_mem_seg_t *_mem_segs;    //pointer to array of structures holding shared mem segment info


/***********************/
/* message queue stuff */
key_t _mq_key = (key_t)99999;
int _msqid_main;

typedef struct _shm_ctx {
    char hdr[15];
    char file_path[512];
    size_t file_size;
    int mem_seg_id;
    size_t mem_seg_tot_sz;
    size_t mem_seg_used_sz;
    int err_stat;
} shm_context_t;

typedef struct _msg_bfr {
    long mtype; //use this to represent message channel
    shm_context_t msg_data;
} shm_msg_bfr_t;

/*************************/
/* function declarations */
int shm_send_simple_msg(int msg_chan, char *msg_hdr);
int shm_send_msg(int msg_chan, shm_context_t *shm_ctx);
int shm_wait_for_msg(int msg_chan, char *msg_hdr, shm_msg_bfr_t *msg_bfr, int iwait);


/**********************************************/
/* MESSAGE CONTEXT STRUCTURE FUNCTIONS        */
/**********************************************/

shm_context_t *shm_context_create(char *file_path, int mem_seg_id) {
    shm_context_t *ctx = malloc(sizeof(shm_context_t));
    bzero(ctx, sizeof(*ctx));
    strncpy(ctx->file_path, file_path, strlen(file_path));
    ctx->mem_seg_id = mem_seg_id;
    ctx->mem_seg_tot_sz = _mem_seg_sz;
    return ctx;
}

void shm_context_cleanup(shm_context_t *ctx) {
    free(ctx);
    ctx = NULL;
}

char *shm_context_get_file_path(shm_context_t *ctx) {
    return ctx->file_path;
}

void shm_context_set_file_size(shm_context_t *ctx, size_t file_sz) {
    ctx->file_size = file_sz;
}

size_t shm_context_get_file_size(shm_context_t *ctx) {
    return ctx->file_size;
}

int shm_context_get_seg_id(shm_context_t *ctx) {
    return ctx->mem_seg_id;
}

size_t shm_context_get_seg_tot_sz(shm_context_t *ctx) {
    return ctx->mem_seg_tot_sz;
}

void shm_context_set_seg_used_sz(shm_context_t *ctx, size_t used_size) {
    ctx->mem_seg_used_sz = used_size;
}

size_t shm_context_get_seg_used_sz(shm_context_t *ctx) {
    return ctx->mem_seg_used_sz;
}

void shm_context_set_error(shm_context_t *ctx, int error) {
    ctx->err_stat = error;
}

int shm_context_get_error(shm_context_t *ctx) {
    return ctx->err_stat;
}


/**********************************************/
/* MESSAGE QUEUE LIBRARY FUNCTIONS            */
/**********************************************/

int shm_init_msg_que() {
    /* try to create message queue) */
    int ret = 0;
    if (dbg_mque) {
        _msqid_main = msgget(_mq_key, 0666 | IPC_CREAT);
    } else {
        _msqid_main = msgget(_mq_key, 0666 | IPC_CREAT);
    }
    if (_msqid_main == -1) {
        perror("[ERROR] server - could not initialize message queue");
        ret = -1;
    }
    return ret;
}

int shm_destroy_msg_que() {
    /* perform any additional cleanup then destroy queue */
    int ret = msgctl(_msqid_main, IPC_RMID, NULL);
    if ( ret != 0) {
        perror("[ERROR] could not destroy message queue");
    }
    return ret;
}

int shm_connect_to_msg_que() {

    fprintf(stderr, "[INFO] client - attempting to connect to message queue ... \n");

    /* wait until queue is established by server */
    int try = 0;
    while (try < WAIT_TRYS) {
        _msqid_main = msgget(_mq_key, 0666 | 0);
        fprintf(stderr, "[INFO] client - msg que id is %d\n", _msqid_main);
        if (_msqid_main < 0) {
            fprintf(stderr, "                message queue not found trying again ...\n");
            sleep(WAIT_TIME_SEC);
            try++;
        } else {
            fprintf(stderr, "                SUCCESS\n");
            break;
        }
    }
    if (try == WAIT_TRYS) {
        fprintf(stderr, "[ERROR] client - tried %d times to connect to message queue, but not successful.\n", try);
        return -1;
    }

    return 0;

}

int shm_send_simple_msg(int msg_chan, char *msg_hdr) {
    shm_context_t msg_data = {{0}};
    shm_msg_bfr_t msg_bfr = {0};
    strncpy(msg_data.hdr, msg_hdr, strlen(msg_hdr));
    msg_bfr.mtype = msg_chan;
    msg_bfr.msg_data = msg_data;
    int ret = msgsnd(_msqid_main, &msg_bfr, sizeof(shm_context_t), 0);
    if (ret == -1) {
        return -1;
    }
    return 0;
}

int shm_send_msg(int msg_chan, shm_context_t *shm_ctx) {
    shm_msg_bfr_t msg_bfr;
    msg_bfr.mtype = msg_chan;
    msg_bfr.msg_data = *shm_ctx;
    int ret = msgsnd(_msqid_main, &msg_bfr, sizeof(shm_context_t), 0);
    if (ret == -1) {
        return -1;
    }
    return 0;
}

int shm_wait_for_msg(int msg_chan, char *msg_hdr, shm_msg_bfr_t *msg_bfr, int iwait) {
    ssize_t ret = 0;
    if (dbg) fprintf(stderr, "INFO: Chan-%d, waiting to receive message with hdr: %s ... \n", msg_chan, msg_hdr);
    int wait = 0;
    if (iwait) wait = 1;
    int try = 0;
    try = 0;
    while (try < WAIT_TRYS) {
        ret = msgrcv(_msqid_main, msg_bfr, sizeof(shm_context_t), msg_chan, (IPC_NOWAIT*wait));
        /* check message text for acknowledgment */
        if (ret == -1 && errno != ENOMSG && errno != EAGAIN) {
            char *msg = NULL;
            sprintf(msg, "[ERROR] Chan-%d, when trying to read message queue", msg_chan);
            perror(msg);
            return -1;
        } else if ( (ret != -1) && (memcmp((*msg_bfr).msg_data.hdr, msg_hdr, strlen(msg_hdr)) != 0) ) {
            fprintf(stderr, "[ERROR] Chan-%d, received message but with unexpected header %s\n", msg_chan, (*msg_bfr).msg_data.hdr);
            return -1;
        } else if ( (ret != -1) && (memcmp((*msg_bfr).msg_data.hdr, msg_hdr, strlen(msg_hdr)) == 0) ) {
            break;
        }
        if (dbg) fprintf(stderr, "INFO: Chan-%d, no message received, trying again ... \n", msg_chan);
        try++;
        sleep(WAIT_TIME_SEC);
    }
    if (try == WAIT_TRYS) {
        if (dbg) fprintf(stderr, "ERROR: Chan-%d, tried %d times to wait for message, but never came.\n", try, msg_chan);
        return -1;
    }
    return 0;
}

/* NOT USED */
int shm_client_handshake() {

    if (dbg) fprintf(stderr, "[INFO] client establishing sync handshake\n");

    ssize_t ret = 0;

    /* send sync message to server */
    ret = shm_send_simple_msg(SHM_MAIN_CHAN_C, SHM_MSG_HDR_SYNC);
    if (ret == -1) {
        perror("[ERROR] client - could not send handshake message\n.");
        return -1;
    };

    /* wait to receive sync-acknowledge from server */
    shm_msg_bfr_t msg_bfr = {0};
    ret = shm_wait_for_msg(SHM_MAIN_CHAN_S, SHM_MSG_HDR_SAKNW, &msg_bfr, SHM_MSG_NOWAIT);
    if (ret == -1) {
        perror("ERROR: client - never received acknowledge request from server\n.");
        return -1;
    };

    /* send acknowledge to server */
    if (dbg) fprintf(stderr, "INFO: client sending acknowledge to server\n");
    ret = shm_send_simple_msg(SHM_MAIN_CHAN_C, SHM_MSG_HDR_CAKNW);
    if (ret == -1) {
        perror("[ERROR] client - could not send handshake acknowledge message\n.");
        return -1;
    };

    if (dbg) fprintf(stderr, "INFO: successful handshake with server\n");
    return 0;

}

int shm_client_send_file_request(shm_context_t *shm_ctx) {
    int ret = 0;
    strncpy(shm_ctx->hdr, SHM_MSG_HDR_RQST, strlen(SHM_MSG_HDR_RQST));
    ret = shm_send_msg(SHM_MAIN_CHAN_C, shm_ctx);
    if (ret == -1) {
        fprintf(stderr, "[ERROR] client - could not submit file request message\n.");
        return -1;
    };
    shm_msg_bfr_t msg_bfr = {0}; //buffer to hold result
    ret = shm_wait_for_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_S+SHM_MAIN_CHAN_OFST, SHM_MSG_HDR_RSPN, &msg_bfr, SHM_MSG_WAIT);
    if (ret == -1) {
        fprintf(stderr, "[ERROR] client - something went wrong tring to read server response.\n.");
        return -1;
    };
    memcpy(shm_ctx, &(msg_bfr.msg_data), sizeof(shm_context_t));
    return 0;
}

int shm_client_wait_for_ready(shm_context_t *shm_ctx) {
    shm_msg_bfr_t msg_bfr = {0};
    int ret = shm_wait_for_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_S+SHM_MAIN_CHAN_OFST, SHM_MSG_HDR_RDY, &msg_bfr, SHM_MSG_WAIT);
    if (ret == 0) {
        memcpy(shm_ctx, &(msg_bfr.msg_data), sizeof(shm_context_t));
    } else if (ret == -1) {
        fprintf(stderr, "[ERROR] client - something went wrong trying to read ready response.\n.");
        return -1;
    }
    return ret;
}

int shm_client_send_acknowledge(shm_context_t *shm_ctx) {
    return shm_send_simple_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_C+SHM_MAIN_CHAN_OFST, SHM_MSG_HDR_CAKNW);
}

/* NOT USED */
int shm_server_handshake() {

    if (dbg) fprintf(stderr, "[INFO] server establishing sync handshake\n");

    ssize_t ret = 0;
    shm_msg_bfr_t msg_bfr = {0};

    /* wait for sync message from client */
    ret = shm_wait_for_msg(SHM_MAIN_CHAN_C, SHM_MSG_HDR_SYNC, &msg_bfr, SHM_MSG_NOWAIT);
    if (ret == -1) {
        perror("[ERROR] server - never received sync request from client\n.");
        return -1;
    };

    /* send sync acknowledged message to client */
    if (dbg) fprintf(stderr, "[INFO] server sending acknowledge message\n");
    ret = shm_send_simple_msg(SHM_MAIN_CHAN_S, SHM_MSG_HDR_SAKNW);
    if (ret == -1) {
        perror("[ERROR] server - could not send acknowledge message from server\n.");
        return -1;
    };

    /* wait for final acknowledgement from client */
    ret = shm_wait_for_msg(SHM_MAIN_CHAN_C, SHM_MSG_HDR_CAKNW, &msg_bfr, SHM_MSG_NOWAIT);
    if (ret == -1) {
        perror("[ERROR] server - never received acknowledge from client\n.");
        return -1;
    };

    if (dbg) fprintf(stderr, "[INFO]] successful handshake with client\n");
    return 0;

}

/* caller of function must free the shm_context */
shm_context_t *shm_server_wait_for_file_request() {
    shm_context_t *shm_ctx = NULL;
    shm_msg_bfr_t msg_bfr = {0};
    int ret = shm_wait_for_msg(SHM_MAIN_CHAN_C, SHM_MSG_HDR_RQST, &msg_bfr, SHM_MSG_WAIT);
    if (ret == 0) {
        shm_ctx = malloc(sizeof(shm_context_t));
        memcpy(shm_ctx, &(msg_bfr.msg_data), sizeof(shm_context_t));
    }
    return shm_ctx;
}

int shm_server_send_response(shm_context_t *shm_ctx) {
    strcpy(shm_ctx->hdr, SHM_MSG_HDR_RSPN);
    return shm_send_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_S+SHM_MAIN_CHAN_OFST, shm_ctx);
}

int shm_server_send_ready(shm_context_t *shm_ctx) {
    strcpy(shm_ctx->hdr, SHM_MSG_HDR_RDY);
    return shm_send_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_S+SHM_MAIN_CHAN_OFST, shm_ctx);
}

int shm_server_wait_for_acknowledge(shm_context_t *shm_ctx) {
    shm_msg_bfr_t msg_bfr = {0};
    int ret = shm_wait_for_msg(shm_ctx->mem_seg_id+SHM_MAIN_CHAN_C+SHM_MAIN_CHAN_OFST, SHM_MSG_HDR_CAKNW, &msg_bfr, SHM_MSG_WAIT);
    if (ret != 0) {
        fprintf(stderr, "[Error] server - something went wrong trying to read acknowledge message.\n");
    }
    return ret;
}


/**********************************************/
/* SHARED MEMORY LIBRARY ELEMENTS             */
/**********************************************/

int shm_init_mem_segs(unsigned short num_segs, size_t seg_sz) {

    int ret = 0;

    _num_mem_segs = num_segs;
    _mem_seg_sz = seg_sz;
    _mem_segs = calloc((size_t)num_segs, sizeof(shm_mem_seg_t));

    int shmid = 0;
    key_t new_key;
    for (int i = 0; i < num_segs; i++) {
        /* connect to (and possibly create) the segment: */
        new_key = _mem_key_seed + (key_t)i;
        if ((shmid = shmget(new_key, seg_sz, 0666 | IPC_CREAT)) == -1) {
            perror("[ERROR] could not create memory segment");
            return -1;
        }
        if (dbg) fprintf(stderr, "[INFO] new memory seg w id: %d\n", shmid);
        _mem_segs[i].seg_id = shmid;
    }
    return ret;
}

int shm_destroy_mem_segs() {
    for (int i = 0; i < _num_mem_segs; i++) {

        /* TODO: need to check for existing locks and wait to proceed */
        if ( shmctl(_mem_segs[i].seg_id, IPC_RMID, NULL) != 0) {
            perror("[ERROR] could not destroy shared memory segment");
            return -1;
        }
    }
    free(_mem_segs);
    _num_mem_segs = 0;
    return 0;
}

void *shm_attach_mem_seg(int mem_seg_id) {
    /* attach memory segment to address */
    void *new_addr = shmat(mem_seg_id, NULL, 0);
    if ( new_addr == (void *)-1) {
        perror("[ERROR] could not attach to memory segment");
        return NULL;
    }
    return new_addr;
}

int shm_detach_mem_seg(void *mem_seg_addr) {
    /* attach memory segment to address */
    int ret = shmdt(mem_seg_addr);
    if ( ret == -1) {
        perror("[ERROR] could not detach to memory segment");
        return -1;
    }
    return 0;
}

ssize_t shm_read_mem_seg(void *mem_seg_addr, char *buffer, size_t buf_len) {
    memcpy(buffer, mem_seg_addr, buf_len);
    return buf_len;
}

ssize_t shm_write_mem_seg(void *mem_seg_addr, char *buffer, size_t buf_len) {
    memcpy(mem_seg_addr, buffer, buf_len);
    return buf_len;
}

int *shm_get_mem_seg_ids(size_t *num_seg_ids) {
    int *seg_ids = calloc(_num_mem_segs, sizeof(int));
    for (int i = 0; i < _num_mem_segs; i++) {
        seg_ids[i] = _mem_segs[i].seg_id;
    }
    *num_seg_ids = _num_mem_segs;
    return seg_ids;
}
