#ifndef _SHM_CHANNEL_H_
#define _SHM_CHANNEL_H_

#define SHM_MSG_HDR_SYNC   "SYNC"
#define SHM_MSG_HDR_SAKNW  "S_AKNW"
#define SHM_MSG_HDR_CAKNW  "C_AKNW"
#define SHM_MSG_HDR_RQST   "RQST"
#define SHM_MSG_HDR_RSPN   "RSPNS"
#define SHM_MSG_HDR_RDY    "RDY"
#define SHM_MSG_HDR_ERR    "ERR"

#define SHM_STAT_OK 200
#define SHM_STAT_NOT_FOUND 404

/**********************************************/
/* MESSAGE CONTEXT STRUCTURE & FUNCTIONS      */
/**********************************************/
typedef struct _shm_ctx shm_context_t;

shm_context_t *shm_context_create(char *file_path, int mem_seg_id);

void shm_context_cleanup(shm_context_t *ctx);

char *shm_context_get_file_path(shm_context_t *ctx);

void shm_context_set_file_size(shm_context_t *ctx, size_t file_sz);

size_t shm_context_get_file_size(shm_context_t *ctx);

int shm_context_get_seg_id(shm_context_t *ctx);

size_t shm_context_get_seg_tot_sz(shm_context_t *ctx);

void shm_context_set_seg_used_sz(shm_context_t *ctx, size_t used_size);

size_t shm_context_get_seg_used_sz(shm_context_t *ctx);

void shm_context_set_error(shm_context_t *ctx, int error);

int shm_context_get_error(shm_context_t *ctx);


/**********************************************/
/* MESSAGE QUEUE LIBRARY ELEMENTS             */
/**********************************************/

/*
 * Creates a single message queue, this function
 *  should be called by the component that is responsible
 *  for creating/destroying the shared memory data channel.  
 */
int shm_init_msg_que();

/*
 * Destroys and clean up the message queue, this function
 *  should be called by the component that is responsible
 *  for creating/destroying the shared memory data channel  
 */
int shm_destroy_msg_que();

/*
 * Connects to an existing message queue, this function
 *  should be called by the component that is not responsible
 *  for the shared memory data channel.  This function should
 *  be called first to ensure that a message queue exists
 *  for communicating with the other component.
 */
int shm_connect_to_msg_que();

/*
 * send request for file to receive from cache
 *  message is sent on main message queue channel
 *  on successfull return (0), shm_ctx will be updated
 *  to contain the file size to be transferred.  On failed
 *  returns only error status will be updated to indicate
 *  the type of error.  The value will be on of the defined
 *  macros above
 */
int shm_client_send_file_request(shm_context_t *shm_ctx);

int shm_client_wait_for_ready(shm_context_t *shm_ctx);
int shm_client_send_acknowledge(shm_context_t *shm_ctx);

shm_context_t *shm_server_wait_for_file_request();
int shm_server_send_response(shm_context_t *shm_ctx);
int shm_server_send_ready(shm_context_t *shm_ctx);
int shm_server_wait_for_acknowledge(shm_context_t *shm_ctx);


/**********************************************/
/* SHARED MEMORY LIBRARY ELEMENTS             */
/**********************************************/

int shm_init_mem_segs(unsigned short num_segs, size_t seg_sz);
int shm_destroy_mem_segs();

void *shm_attach_mem_seg(int mem_seg_id);
int shm_detach_mem_seg(void *mem_seg_addr);

ssize_t shm_read_mem_seg(void *mem_seg_addr, char *buffer, size_t buf_size);
ssize_t shm_write_mem_seg(void *mem_seg_addr, char *buffer, size_t buf_size);

int *shm_get_mem_seg_ids(size_t *num_seg_ids);

#endif