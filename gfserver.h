#ifndef __GETFILE_SERVER_H__
#define __GETFILE_SERVER_H__

#include <pthread.h>
#include "steque.h"

#define  GF_OK 200
#define  GF_FILE_NOT_FOUND 404
#define  GF_ERROR 500

typedef int gfstatus_t;

/**************/
/* structures */
/**************/
typedef struct _gfcontext_t gfcontext_t;
typedef struct _gfserver_t {
    unsigned short port;                                //socket port number
    int max_pend;                                       //maximum number of pending connections
    ssize_t (*hndlr_func)(gfcontext_t*, char*, void*);  //function callback for handling the data the be sent
    void *hndlr_arg;                                    //argument to handler function callback
    unsigned short nwrkr_thds;
} gfserver_t;

typedef struct _gfcontext_t {
    int sockfd;                    //socket file descriptor
    gfstatus_t stat;               //current error status of context
    struct sockaddr_in* cli_addr;  //socket address of the connected client
    socklen_t cli_addr_len;        //socket address length of connected client
    gfserver_t *gfs;               //pointer to gfserver structure required for calling request handler with arguments
} gfcontext_t;

/* 
 * This function must be the first one called as part of 
 * setting up a server.  It returns a gfserver_t handle which should be
 * passed into all subsequent library calls of the form gfserver_*.  It
 * is not needed for the gfs_* call which are intended to be called from
 * the handler callback.
 */
void gfserver_init(gfserver_t *gfs, unsigned short nwrkr_thds);

/*
 * Sets the port at which the server will listen for connections.
 */
void gfserver_set_port(gfserver_t *gfs, unsigned short port);

/*
 * Sets the maximum number of pending connections which the server
 * will tolerate before rejecting connection requests.
 */
void gfserver_set_maxpending(gfserver_t *gfs, int max_npending);

/*
 * Sets the number of worker threads to be used for concurrently
 * handling requests.
 */
void gfservers_set_num_threads(gfserver_t *gfs, int n_wrkr_thds);

/*
 * Sets the handler callback, a function that will be called for each each
 * request.  As arguments, this function receives:
 * - a gfcontext_t handle which it must pass into the gfs_* functions that 
 * 	 it calls as it handles the response.
 * - the requested path
 * - the pointer specified in the gfserver_set_handlerarg option.
 * The handler should only return a negative value to signal an error.
 */
void gfserver_set_handler(gfserver_t *gfs, ssize_t (*handler)(gfcontext_t *, char *, void*));

/*
 * Sets the third argument for calls to the handler callback.
 */
void gfserver_set_handlerarg(gfserver_t *gfs, void* arg);

/*
 * Starts the server.  Does not return.
 */
void gfserver_serve(gfserver_t *gfs);

/*
 * Stops the server and cleans up
 */
void gfserver_stop(gfserver_t *gfs);


/*
 * Sends to the client the Getfile header containing the appropriate 
 * status and file length for the given inputs.  This function should
 * only be called from within a callback registered gfserver_set_handler.
 */
ssize_t gfs_sendheader(gfcontext_t *ctx, gfstatus_t status, size_t file_len);

/*
 * Sends size bytes starting at the pointer data to the client 
 * This function should only be called from within a callback registered 
 * with gfserver_set_handler.  It returns once the data has been
 * sent.
 */
ssize_t gfs_send(gfcontext_t *ctx, void *data, size_t size);

/*
 * Aborts the connection to the client associated with the input
 * gfcontext_t.
 */
void gfs_abort(gfcontext_t *ctx);


#endif
