# myWebServer C++ linux服务器

​	这是一个用C++编写的简单的linux服务器。

## 1.服务器原理

​		主线程循环调用epoll_wait函数监听文件描述符上是否有事件发生，当接收到客户端的http请求时为其分配工作线程。工作线程解析客户端发来的请求并构造http响应报文。使用定时器处理非活动连接。

### 线程池

​	 线程池相关的代码在threadpool.h文件中，在线程池构造函数中使用pthread_create函数对每个线程初始化，运行的线程函数为threadpool类的静态成员函数worker 。

​	 threadpool类有一个私有成员变量m_workqueue表示要处理的工作队列。worker 函数会调用该threadpool对象的run函数，不断从工作队列中取出工作执行。下面run函数执行的request->process(), 即为http_conn类处理客户端请求的函数。

​	线程池在访问工作队列m_workqueue使用互斥锁保证了线程安全。

```C++
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        
        //printf("get request\n");
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        request->process();
        
    }
}
```

### 定时器

​	  本项目定时器相关的代码在time_heap.h, time_heap.cpp中，使用定时器处理非活动连接，每个连接绑定一个定时器，当连接长时间没有发送或接收数据，就会被定时器的回调函数cb_func关闭，并注销其在内核事件表中的登记。

```C++
void cb_func(client_data* user_data){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    //printf("cbfunc close fd %d\n", user_data->sockfd);
}
```

​		本项目使用小根堆作为定时器容器，这样的好处是超时值最小的定时器必然在堆顶，每次只要检查堆顶的定时器是否超时。这样执行一次定时器的时间复杂度为O(1), 但执行完后弹出堆顶的定时器调整小根堆的时间复杂度为O(lgn ),  添加一个定时器的复杂度为O(lgn),但本项目删除定时器采用延迟销毁，时间复杂度为O(1)。

### 主线程 

```C++
while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		...
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
               //建立连接
               //为连接分配定时器

            }
            else if((sockfd=pipefd[0])&&(events[i].events&EPOLLIN)){
                //处理信号，信号通过管道写入，和客户端连接的读写区分开来

            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //发生异常，关闭对应的定时器
                
            }
            else if (events[i].events & EPOLLIN)
            {
                //接收客户端的请求，为其分配工作线程处理
                //修改定时器的超时值
            }
            else if (events[i].events & EPOLLOUT)
            {
                //向客户端发送http响应报文
            else
            {
            }
            if(timeout){
                timer_handler();
                timeout=false;
            }
        }
    }
```

### 工作线程

实际执行的是http_conn类的process函数,调用process_read函数解析http请求报文，调用process_write函数构造http响应报文。

```C++
void http_conn::process()
{
    //printf("now read\n");
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        //printf("write error\n");
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
```

## 2.运行服务器

需要在linux系统下安装了g++编译器,

运行build.sh生成可执行文件myWebServer

运行myWebServer， 默认监听0.0.0.0（本机上的所有网卡），运行在9090端口

也可运行   ./myWebServer [ip] [port]  指定ip和端口

运行结果如下：

会显示客户端传来的每一行http请求

![img4.png](https://github.com/windrider/myWebServer/blob/main/imgs/img4.png?raw=true)

用firefox浏览器访问服务器：

![img1.png](https://github.com/windrider/myWebServer/blob/main/imgs/img1.png?raw=true)

## 3.性能测试

使用apachebench，模拟1000台客户端测试10秒。

在自己电脑的ubuntu18虚拟机上运行并测试的结果如下

![img2.png](https://github.com/windrider/myWebServer/blob/main/imgs/img2.png?raw=true)

![img3.png](https://github.com/windrider/myWebServer/blob/main/imgs/img3.png?raw=true)