#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <signal.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "time_heap.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 20

static int pipefd[2];
static time_heap ti_heap(10000);
static client_data* clients=new client_data[MAX_FD];
static int epollfd;

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

void sig_handler(int sig){
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg ,1,0 );
    errno=save_errno;
}

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler(){
    ti_heap.tick();
    alarm(TIMESLOT);
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

void cb_func(client_data* user_data){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    //printf("cbfunc close fd %d\n", user_data->sockfd);
}

int main(int argc, char *argv[])
{
    /*
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
    }
    */
    const char *ip = "0.0.0.0";
    int port = 9090;
    if (argc >= 3)
    {
        ip = argv[1];
        port = atoi(argv[2]);
    }

    addsig(SIGPIPE, SIG_IGN);
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    struct linger tmp = {1, 0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    ret=socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret!=-1);
    setnonblocking(pipefd[0]);
    addfd(epollfd,pipefd[0],false);
    
    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);
    bool timeout=false;
    bool stop_server=false;
    alarm(TIMESLOT);

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                //printf("get connection, connfd is %d\n",connfd);
                users[connfd].init(connfd, client_address);

                clients[connfd].address=client_address;
                clients[connfd].sockfd=connfd;
                heap_timer* timer=new heap_timer(3*TIMESLOT);
                timer->user_data=&clients[connfd];
                timer->cb_func=cb_func;
                clients[connfd].timer=timer;
                ti_heap.add_timer(timer);

            }
            else if((sockfd==pipefd[0])&&(events[i].events&EPOLLIN)){
                int sig;
                char signals[1024];
                ret=recv(pipefd[0],signals,sizeof(signals),0);
                if(ret==-1||ret==0){
                    continue;
                }
                else{
                    for(int i=0;i<ret;++i){
                        switch(signals[i]){
                            case SIGALRM:{
                                timeout=true;  //deal with i/o task first, deal with timer in the end 
                                break;
                            }
                            case SIGTERM:{
                                stop_server=true;
                            }
                        }
                    }

                }

            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                if(events[i].events & EPOLLRDHUP)("EPOLLRDHUP\n");
                if(events[i].events & EPOLLHUP)("EPOLLHUP\n");
                if(events[i].events & EPOLLERR)("EPOLLERR\n");
                cb_func(&clients[sockfd]);
    
                heap_timer *timer = clients[sockfd].timer;
                if (timer)
                {
                    ti_heap.del_timer(timer);
                }
                
            }
            else if (events[i].events & EPOLLIN)
            {
                heap_timer* timer=new heap_timer(clients[sockfd].timer);
                if (users[sockfd].read())
                {
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        //printf("connection %d adjust timer\n",sockfd);
                        ti_heap.add_timer(timer);
                        ti_heap.del_timer(clients[sockfd].timer);
                    }
                    pool->append(users + sockfd);
                }
                else
                {
                  
                    cb_func(&clients[sockfd]);
                    if (timer)
                    {
                        ti_heap.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                heap_timer* timer=new heap_timer(clients[sockfd].timer);
                if (users[sockfd].write())
                {
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        //printf("connection %d adjust timer\n",sockfd);
                        ti_heap.add_timer(timer);
                        ti_heap.del_timer(clients[sockfd].timer);
                    }
                }
                else{
               
                    cb_func(&clients[sockfd]);
                    if (timer)
                    {
                        ti_heap.del_timer(timer);
                    }
                }
            }
            else
            {
            }
            if(timeout){
                timer_handler();
                timeout=false;
            }
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}
