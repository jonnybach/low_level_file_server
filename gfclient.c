#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h> 
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <stddef.h>

#include "gfclient.h"

#define BUFSIZE 4096
#define SOCKTO 50

static const int dbg = 1;

/*-----------------*/
/* local constants */

static const char *scheme = "GETFILE";
static const char *mthd_get = "GET";
//static const char *mthd_head = "HEAD";
static const char *stat_ok = "OK";
static const char *stat_fnf = "FILE_NOT_FOUND";
static const char *stat_er = "ERROR";
static const char *stat_inv= "INVALID";
static const char *mrkr = "\r\n\r\n";


/*------------*/
/* structures */

struct gfcrequest_t {
    char *serv_name;                            //name of server to connect to
    char *file_path;                            //path of file to be requested for download
    unsigned short port;                        //socket port number
    void (*hdr_func)(void*, size_t, void*);     //function callback when header is recieved in request
    void *hdr_arg;                              //argument to header callback
    void (*write_func)(void *, size_t, void *);  //function callback for each chunk of data received
    void *write_arg;                            //argument to write function callback
    gfstatus_t status;                          //status of the response as returned by the server
    size_t file_len;                            //file length in bytes as returned in header response
    size_t tot_byts_rec;
};


/* functions */

char* gfc_strstatus(gfstatus_t status) {
    char *cstat;
    switch (status) {
        case GF_OK:
            cstat = (char *)stat_ok;
            break;
        case GF_FILE_NOT_FOUND:
            cstat = (char *)stat_fnf;
            break;
        case GF_ERROR:
            cstat = (char *)stat_er;
            break;
        default:
            cstat = (char *)stat_inv;
            break;
    }
    return cstat;
}


gfcrequest_t *gfc_create() {
    gfcrequest_t *gfcr = malloc(sizeof(struct gfcrequest_t));
    bzero(gfcr, sizeof(*gfcr));
    gfcr->tot_byts_rec = 0;
    gfcr->status = GF_OK;
    gfcr->file_len = 0;
    gfcr->port = 0;
    return gfcr;
}

void gfc_set_server(gfcrequest_t *gfr, char* server) {
    gfr->serv_name = strdup(server);
}

void gfc_set_path(gfcrequest_t *gfr, char* path) {
    gfr->file_path = strdup(path);
}

void gfc_set_port(gfcrequest_t *gfr, unsigned short port) {
    gfr->port = port;
}

void gfc_set_headerfunc(gfcrequest_t *gfr, void (*headerfunc)(void*, size_t, void *)) {
    gfr->hdr_func = headerfunc;
}

void gfc_set_headerarg(gfcrequest_t *gfr, void *headerarg) {
    gfr->hdr_arg = headerarg;
}

void gfc_set_writefunc(gfcrequest_t *gfr, void (*writefunc)(void*, size_t, void *)) {
    gfr->write_func = writefunc;
}

void gfc_set_writearg(gfcrequest_t *gfr, void *writearg) {
    gfr->write_arg = writearg;
}

void gfc_set_status(gfcrequest_t *gfr, gfstatus_t status) {
    gfr->status = status;
}

gfstatus_t gfc_get_status(gfcrequest_t *gfr) {
    return gfr->status;
}

size_t gfc_get_filelen(gfcrequest_t *gfr) {
    return gfr->file_len;
}

size_t gfc_get_bytesreceived(gfcrequest_t *gfr) {
    return gfr->tot_byts_rec;
}

ssize_t gfc_get_bytesremaining(gfcrequest_t *gfr) {
    return (gfr->file_len - gfr->tot_byts_rec);
}

void gfc_cleanup(gfcrequest_t *gfr) {
    free(gfr->serv_name);
    free(gfr->file_path);
    free(gfr);
    gfr = NULL;
}

void gfc_global_init() {
}

void gfc_global_cleanup() {
}

int gfc_perform(gfcrequest_t *gfr) {

    /* create socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] opening socket");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }

    /* set socket option to reuse addresses */
    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        fprintf(stderr, "[ERROR] setting port reuse option in setsockopt.\n");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }

    /* set socket timeout options */
    struct timeval timeout;
    timeout.tv_sec = SOCKTO;
    timeout.tv_usec = 0;
    if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
                    sizeof(timeout)) < 0) {
        fprintf(stderr, "[ERROR] setting receive timeout in setsockopt\n");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }
    if (setsockopt (sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                    sizeof(timeout)) < 0) {
        fprintf(stderr, "[ERROR] setting send timeout in setsockopt\n");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }
    /* get server information from hostname provided during command call */
    struct hostent *server = gethostbyname(gfr->serv_name);
    if (server == NULL) {
        fprintf(stderr, "[ERROR], host with provided name cannot be found: %i\n", h_errno);
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }

    /* prepare server address structure */
    struct sockaddr_in serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *) &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(gfr->port);

    /* connect to the server */
    if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("[ERROR] connecting to server");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }

    ssize_t bytes_recv, bytes_sent;
    char buffer[BUFSIZE];

    /* send request for file transfer */
    fprintf(stderr, "[INFO] sending request to server\n");
    bzero(buffer, sizeof(buffer));
    sprintf(buffer, "%s %s %s%s", scheme, mthd_get, gfr->file_path, mrkr);
    bytes_sent = send(sockfd, buffer, strlen(buffer) + 1, 0);
    if (bytes_sent < 0) {
        perror("[ERROR] sending transfer request to server");
        gfc_set_status(gfr, GF_INVALID);
        return -1;
    }

    /* read response from server and possibly perform transfer */
    size_t npos = 0;
    int got_hdr = 0;
    int hdr_schm_vald = 0;
    int no_hdr_err = 0;
    int got_data = 0;

    bzero(buffer, BUFSIZE);

    char *hdr_stuff = calloc(BUFSIZE, sizeof(char));
    size_t hdr_stuff_sz = BUFSIZE;
    size_t hdr_stuff_usd = 0;
    char *hdr_end = hdr_stuff;

    char *cstat;
    char cfilelen[32];
    bzero(cfilelen, sizeof(cfilelen));

    gfr->tot_byts_rec = 0;

    //int n_rds = 0;

    fprintf(stderr, "[INFO] getting response from server\n");
    while ((bytes_recv = recv(sockfd, buffer, BUFSIZE, 0)) > 0) {

        if (!got_hdr) {

            //keep reading bytes and storing in header buffer until
            // marker is found.

            if ((hdr_stuff_sz - hdr_stuff_usd) < bytes_recv) {
                hdr_stuff_sz += BUFSIZE;
                hdr_stuff = realloc(hdr_stuff, hdr_stuff_sz);
            }
            memcpy(&hdr_stuff[hdr_stuff_usd], buffer, (size_t) bytes_recv);
            hdr_stuff_usd += bytes_recv;

            hdr_end = strstr(hdr_stuff, mrkr);
            if (hdr_end != NULL) { /* received full header */

                got_hdr = 1;

                /* check that first 7 chars provide scheme */
                if (memcmp(&hdr_stuff[0], scheme, strlen(scheme)) != 0) {
                    gfc_set_status(gfr, GF_INVALID);
                    break;
                }
                hdr_schm_vald = 1;

                /* place buffer start location after scheme */
                npos = 1 + strlen(scheme); //1 accounts for whitespace between scheme and status

                /* check status for errors */
                cstat = gfc_strstatus(GF_ERROR);
                if (memcmp(&hdr_stuff[npos], cstat, strlen(cstat)) == 0) {
                    gfc_set_status(gfr, GF_ERROR);
                    break;
                }

                cstat = gfc_strstatus(GF_FILE_NOT_FOUND);
                if (memcmp(&hdr_stuff[npos], cstat, strlen(cstat)) == 0) {
                    gfc_set_status(gfr, GF_FILE_NOT_FOUND);
                    break;
                }

                /* no expected errors, status must be OK */
                cstat = gfc_strstatus(GF_OK);
                if (memcmp(&hdr_stuff[npos], cstat, strlen(cstat)) != 0) {
                    fprintf(stderr, "[ERROR] status received from server is unknown, expected OK\n");
                    gfc_set_status(gfr, GF_ERROR);
                    break;
                }

                /* valid header with no errors, get file length */
                no_hdr_err = 1;
                npos += (1 + strlen(cstat));
                memcpy(cfilelen, &hdr_stuff[npos], (hdr_end - &hdr_stuff[npos]));
                gfr->file_len = (size_t) atoi(cfilelen);

                /* get full header length */
                hdr_end += 3;
                ptrdiff_t hdr_len = hdr_end - hdr_stuff + 1;

                /* provide header to header callback */
                if (gfr->hdr_func != NULL)
                    gfr->hdr_func(hdr_stuff, (size_t)hdr_len, gfr->hdr_arg);

                /* anything left in hdr is file contents, provide to write callback */
                size_t byts_lft = hdr_stuff_usd - hdr_len;
                /*determine how many bytes are left to be read and only read from buffer any file bytes remaining */
                size_t byts_to_rd = 0;
                ssize_t byts_rem = gfr->file_len;
                if (byts_rem < byts_lft) {
                    byts_to_rd = (size_t)byts_rem;
                } else {
                    byts_to_rd = (size_t)byts_lft;
                }

                if (byts_lft) {
                    if (dbg) fprintf(stderr, "[INFO] writing any additional data bytes after parsing header\n");
                    gfr->tot_byts_rec += byts_to_rd;
                    gfr->write_func(hdr_end+1, (size_t) byts_to_rd, gfr->write_arg);
                }

                /* if  received all file bytes stop reading data */
                if (gfc_get_bytesremaining(gfr) <= 0)
                    break;

            } //end check for receiving full header

        } else {

            //if (n_rds < 100) {
            //    fprintf(stderr, "[INFO] reading data file bytes: %i\n", n_rds);
            //    n_rds++;
            //}

            /* determine how many bytes are left to be read and only read from buffer any file bytes reamining */
            size_t byts_to_rd = 0;
            ssize_t byts_rem = gfc_get_bytesremaining(gfr);
            if (byts_rem < bytes_recv) {
                byts_to_rd = (size_t)byts_rem;
            } else {
                byts_to_rd = (size_t)bytes_recv;
            }

            /* receiving data, send bytes to write call back */
            gfr->tot_byts_rec += byts_to_rd;
            gfr->write_func(buffer, (size_t) byts_to_rd, gfr->write_arg);

            /* if  received all file bytes stop reading data */
            if (gfc_get_bytesremaining(gfr) <= 0)
                break;
        }
    }

    if (dbg) fprintf(stderr, "[INFO] done receiving data\n");
    close(sockfd);
    free(hdr_stuff);

    /* check all expected file bytes been received */
    if (gfr->tot_byts_rec == gfr->file_len) {
        got_data = 1;
    }

    int cnct_stat;
    if (bytes_recv == -1) {
        perror("[ERROR] abnormally occurred in receiving response from server");
        gfc_set_status(gfr, GF_INVALID); //didn't have a chance to set this above
        cnct_stat = -1;
    } else {
        /* check that connection terminated gracefully but haven't fully received the header and data file */
        if (!got_hdr) {
            fprintf(stderr, "[ERROR] connection terminated normally BUT header was not fully received\n");
            gfc_set_status(gfr, GF_INVALID); //didn't have a chance to set this above
            cnct_stat = -1;
        } else if (got_hdr && !hdr_schm_vald) {
            fprintf(stderr, "[ERROR] connection terminated normally BUT header scheme does not match GETFILE\n");
            cnct_stat = -1;
        } else if (got_hdr && hdr_schm_vald && !no_hdr_err) {
            fprintf(stderr, "[ERROR] connection terminated normally BUT header contains error report, status: %s\n", gfc_strstatus(gfr->status));
            cnct_stat = 0;
        } else if (got_hdr && hdr_schm_vald && no_hdr_err && !got_data) {
            fprintf(stderr, "[ERROR] connection terminated normally BUT data was not fully received\n");
            cnct_stat = -1;
        } else {
            if (dbg) fprintf(stderr, "[INFO] connection terminated normally AND data successfully transferred\n");
            cnct_stat = 0;
        }
    }

    return cnct_stat;

}