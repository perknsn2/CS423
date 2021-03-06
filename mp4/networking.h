
#ifndef NETWORKING_H
#define NETWORKING_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 




#define ip_address "127.0.0.1"
#define RtoL_DATA_PORT 4200
#define RtoL_STATE_PORT 4201
#define LtoR_DATA_PORT 5200
#define LtoR_STATE_PORT 5201

typedef enum {JOB_TRANSFER, STATE_TRANSFER} n_state;

int join_channel(const char * address, int port);
int create_channel(int port);

int channel_write(int fd, n_state state,void * buf, int nbytes);
n_state channel_read(int fd, void * buf, int nbytes);

void error(const char *msg);

#endif
