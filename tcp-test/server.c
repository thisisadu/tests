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

int cfd;
event_t *cevt;

void r_proc(eloop_t *loop,event_t *evt,long fd,void* arg)
{
  static long count = 0;
	printf("r_proc %ld\n",++count);
  char buf[50];
  read(fd,buf,sizeof(buf));
  write(fd,buf,sizeof(buf));
}

void l_proc(eloop_t *loop,event_t *evt,long fd,void* arg)
{
	printf("l_proc\n");
  cfd = accept(fd, NULL,NULL);
  if(cfd < 0) {
    printf("accept error %d \n", cfd);
    return;
  }

	cevt = e_event_new(E_READ,cfd,r_proc,NULL);
	e_event_add(loop,cevt);
}

int main(int argc,char**argv)
{
  struct sockaddr_in addr;
	int fd = socket(AF_INET,SOCK_STREAM,0);
  memset(&addr,0,sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(5050);
  addr.sin_addr.s_addr=inet_addr("0.0.0.0");
  if(bind(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) < 0) {
    printf("bind error \n");
    close(fd);
    return -1;
  }

  if(listen(fd, 5) < 0) {
    printf("listen error \n");
    close(fd);
    return -1;
  }

	eloop_t *loop = e_loop_new();
	event_t *rnode = e_event_new(E_READ,fd,l_proc,NULL);

	e_event_add(loop,rnode);

	e_loop_run(loop);

	e_event_free(rnode);
  e_loop_free(loop);

  close(fd);
	return 0;
}
