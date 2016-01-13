#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/epoll.h>
#include "log.h"
#include "connobj.h"
#include "server.h"
#include "cstr.h"

ServerObj * StartServer(char *ip,unsigned short port,ProcRead procread)
 {
	ServerObj *serverobj = NULL;

	if (serverobj == NULL) {
		serverobj = Server_Create(1024);

		if (NULL != serverobj) {

			memcpy(serverobj->connobj->ip, ip, sizeof(serverobj->connobj->ip));

			serverobj->connobj->port = port;
			serverobj->procread      = procread;/*注册调用者接收回调函数*/

			Server_Listen(serverobj);

			Threadpool_Addtask(serverobj->datathread,&Server_Process,
							"Server_Process", serverobj);

			Threadpool_Addtask(serverobj->serverthread,&Server_Loop,
					"Server_Loop", serverobj);
		}
	}

	return serverobj;
}

ServerObj *Server_Create(int events)
{
	ServerObj *serverobj = NULL;

	serverobj = (ServerObj *)malloc(sizeof(ServerObj));

	if (NULL != serverobj){

		serverobj->epollobj     = Epoll_Create_Obj(events);
		serverobj->connmgr      = ConnMgr_Create();
		serverobj->lockerobj    = LockerObj_Create();
		serverobj->connobj      = CreateNewConnObj();
		serverobj->rqueue       = DataQueue_Create();
		serverobj->serverthread = Threadpool_Create(1);/*Proactor 模式，单线程*/
		serverobj->datathread   = Threadpool_Create(5);/*数据线程池，处理接收数据用的*/

		serverobj->connmgr->reset(serverobj->connobj);/*初始化socket连接对象*/
	}

	return serverobj;
}

void Server_Clear(ServerObj *serverobj)
{
    if (serverobj != NULL){
    	Threadpool_Destroy(serverobj->datathread);
    	Threadpool_Destroy(serverobj->serverthread);
    	ConnMgr_Clear(serverobj->connmgr);
    	Locker_Lock(serverobj->lockerobj->locker);
    	if (serverobj->connobj != NULL){
    		free(serverobj->connobj);
    		serverobj->connobj = NULL;
    	}
    	Locker_Unlock(serverobj->lockerobj->locker);
    	Epoll_Destory_Obj(serverobj->epollobj);
    }
}

int Server_Listen(ServerObj *serverobj)
{
	int sock = -1;
	int opt  = 1;

	struct sockaddr_in addr;

	if (NULL == serverobj) {
		return -1;
	}

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((unsigned short)serverobj->connobj->port);
	addr.sin_addr.s_addr = inet_addr(serverobj->connobj->ip);

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		goto sock_err;
	}

	serverobj->connobj->fd = sock;

	serverobj->connobj->noblock(serverobj->connobj,1);
	serverobj->connobj->keepalive(serverobj->connobj,1);

	if (setsockopt(serverobj->connobj->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
		goto sock_err;
	}

	if (bind(serverobj->connobj->fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		log_error("bind socket fail!socket:%d,error:%s",serverobj->connobj->fd,strerror(errno));
		goto sock_err;
	}

	if (listen(serverobj->connobj->fd, 1024) == -1) {
		goto sock_err;
	}

	serverobj->epollobj->add(serverobj->epollobj->epollbase,serverobj->connobj,EPOLLIN);

	return 0;

sock_err:

	if(serverobj->connobj->fd >= 0){
		serverobj->connobj->close(serverobj->connobj);
	}
	return -1;
}

ConnObj  *Server_Accept(ServerObj *serverobj)
{
	int client_sock;
    struct sockaddr_in addr;

	socklen_t addrlen  = sizeof(addr);
	ConnObj  *_connobj = NULL;
	struct linger opt  = {1,0};

	while((client_sock = accept(serverobj->connobj->fd, (struct sockaddr *)&addr, &addrlen)) == -1){
		if(errno != EINTR){
			return NULL;
		}
	}

	setsockopt(client_sock, SOL_SOCKET, SO_LINGER, (void *)&opt, sizeof(opt));

	_connobj = serverobj->connmgr->get(serverobj->connmgr);

	if (_connobj != NULL){

		memcpy(_connobj->ip,inet_ntoa(addr.sin_addr),sizeof(_connobj->ip));

		_connobj->port      = ntohs(addr.sin_port);
		_connobj->last_time = time(NULL);
		_connobj->fd        = client_sock;
		_connobj->activity  = SOCKET_CONNECTING;

		_connobj->keepalive(_connobj,1);
		_connobj->nodelay(_connobj,1);
		_connobj->noblock(_connobj,1);

		if (serverobj->epollobj->add(serverobj->epollobj->epollbase,_connobj,EPOLLIN) !=0 ){/*设置客户端socket epoll事件*/
           /*add fail*/
			serverobj->connmgr->set(serverobj->connmgr,_connobj);/*把连接对象，放回到连接池中*/
			goto sock_err;
		}

		log_info("create net connect:addr;%p,fd:%d\n",_connobj,_connobj->fd);

	}else{
		goto sock_err;
	}

	return _connobj;

sock_err:
	if (client_sock != 0){
		close(client_sock);
		return NULL;
	}
    return NULL;
}
int  ServerSend(ServerObj *serverobj,ConnObj *connobj,char *data,int len)
{
	char *dptr   = NULL;
	int   result = 0;

    if (NULL != connobj && serverobj != NULL && len > 0 && connobj->activity == SOCKET_CONNECTED){

     	dptr = CStr_Malloc((char *)data,len);

    	connobj->sendptr = (unsigned char *)dptr;
    	connobj->sendlen = len;
    }

    return result;
}

void Server_Process(void *argv)
{
	ServerObj *serverobj = (ServerObj *) argv;

	Item *item = NULL;

	for (;;) {

		if (NULL != serverobj) {

			Locker_Semwait(serverobj->lockerobj->locker);
			Locker_Lock(serverobj->lockerobj->locker);

			if (DataQueue_Size(serverobj->rqueue) > 0){
				item = DataQueue_Pop(serverobj->rqueue);
				if (item != NULL){

					serverobj->procread(item->connobj,item->recvptr,item->recvlen);

					if (item->recvptr != NULL) {
						CStr_Free(item->recvptr);
						item->recvptr = NULL;
				    }

					Epoll_Event_ModifyConn(serverobj->epollobj->epollbase, item->connobj,EVENT_WRITE|EPOLLERR|EPOLLONESHOT);

					free(item);
					item = NULL;
				}
			}
			Locker_Unlock(serverobj->lockerobj->locker);
		}
	}
}

void Server_Loop(void *argv)
{
	ServerObj *serverobj = (ServerObj *)argv;

	if (NULL != serverobj){

		for(;;){
			serverobj->epollobj->wait(serverobj->epollobj->epollbase,serverobj,1);
		}
	}
}
