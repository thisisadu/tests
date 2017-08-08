#include "eloop.h"
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h> /* inet(3) functions */
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int rcount = 0; 
static int wcount = 0; 
static struct timeval rot; 
static struct timeval wot; 

void r_proc(eloop_t *loop,event_t *evt,long fd,void* arg)
{
	/* printf("r_proc\n"); */
  char buf[640];
  read(fd,buf,sizeof(buf));
  rcount ++;
  struct timeval tv;
  gettimeofday(&tv,NULL);
  if(tv.tv_sec - rot.tv_sec >=1){
    printf("**********************************************************recv %d pkts in last second\n",rcount);
    rcount = 0;
    rot = tv;
  }
}

void w_proc(eloop_t *loop,event_t *evt,long fd,void* arg)
{
	/* printf("w_proc\n"); */
  char buf[640];
  strcpy(buf,"640 bytes data\n");
  write(fd,buf,sizeof(buf));
  wcount ++;
  struct timeval tv;
  gettimeofday(&tv,NULL);
  if(tv.tv_sec - wot.tv_sec >=1){
    printf("**********************************************************send %d pkts in last second\n",wcount);
    wcount = 0;
    wot = tv;
  }
}

int main(int argc,char**argv)
{
  if(argc<2){
    printf("usage:./c ip\n");
    return -1;
  }

  struct sockaddr_in addr;
	int fd = socket(AF_INET,SOCK_STREAM,0);
  memset(&addr,0,sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(5050);
  addr.sin_addr.s_addr=inet_addr(argv[1]);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) < 0) {
    printf("connect error \n");
    close(fd);
    return -1;
  }

  gettimeofday(&rot,NULL);
  gettimeofday(&wot,NULL);

	eloop_t *loop = e_loop_new();
	event_t *rnode = e_event_new(E_READ,fd,r_proc,NULL);
	event_t *wnode = e_event_new(E_WRITE,fd,w_proc,NULL);

	e_event_add(loop,rnode);
	e_event_add(loop,wnode);

	e_loop_run(loop);

	e_event_free(rnode);
	e_event_free(wnode);
  e_loop_free(loop);

  close(fd);
	return 0;
}
