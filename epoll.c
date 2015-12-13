#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "connmgr.h"
#include "epoll.h"

EpollBase *Init_EpollBase(int events)
{
	EpollBase *evb = NULL;

	struct epoll_event *event = NULL;

	int ephandle = 0;
	ephandle     = epoll_create(events);

	if (ephandle < 0) {
		printf("epoll create of size:%d failed! %s",events,strerror(errno));
		return NULL;
	}

	event = malloc(events * sizeof(struct epoll_event));

    if (event == NULL){
    	return NULL;
    }

	evb = (EpollBase *)malloc(sizeof(EpollBase));

	if (evb != NULL){
        evb->epollhandle = ephandle;
        evb->cb          = Epoll_Event_Callback;
        evb->event       = event;
	}

	return evb;
}

void Clear_EpollBase(EpollBase *evb)
{
	if (evb != NULL){
        close(evb->epollhandle);
        if (evb->event != NULL){
        	free(evb->event);
        }
        free(evb);
        evb = NULL;
	}
}
int Epoll_Event_Callback(struct tagConnObj  *conn,int events)
 {
	int val = 0;
	errno   = 0;
	socklen_t lon = sizeof(int);
	int ret = 0;
	unsigned char recvbuffer[1024*16] = {0};

	int  recvlen = 0;
	int  datalen = 0;

	if (NULL == conn) {
		return -1;
	}

	if (conn->activity == SOCKET_CONNECTING) { /*正在连接*/

		ret = getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (void *) &val,&lon);

		if (ret == -1) {
			printf("connect getsockopt() errno %d, %s, fd %d",errno,strerror(errno),conn->fd);
			return -1;
		}

		if (val == 0){
			conn->activity = SOCKET_CONNECTED;
		}else{
			conn->activity = SOCKET_CONNCLOSED;
		}

		if (val != 0 ){
			close(conn->fd);
		}
	}

	if (EPOLLIN & events){ /*检测到读事件*/

		recvlen = conn->recv(conn,recvbuffer,sizeof(recvbuffer));
		if (recvlen <0){
			recvlen = conn->recv(conn,recvbuffer,sizeof(recvbuffer));
			if (recvlen >0){
				datalen += recvlen;
			}
		}else{
			datalen += recvlen;
		}

		memcpy(conn->recvptr,recvbuffer,recvlen);
		conn->recvlen = datalen;
	}

	if (EPOLLOUT & events){ /*检测到写事件*/
		if (conn->sendptr != NULL && conn->sendlen > 0 ){
			datalen = conn->send(conn);
		}
	}

	return datalen;
}


