#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

// 信号处理函数
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );

    // 对SIGPIPE信号进行处理
    // 如果一个 socket 在接收到了 RST packet 之后，程序仍然向这个 socket 写入数据，那么就会产生SIGPIPE信号。
    // 这种现象是很常见的，譬如说 server 准备向 client 发送多条消息，但在发送消息之前，client 进程意外奔溃了
    // ，那么接下来 server 在发送多条消息的过程中，就会出现SIGPIPE信号。
    addsig( SIGPIPE, SIG_IGN );

    // 创建线程池，初始化线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    // 创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[MAX_FD];

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    // 绑定
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create(5);
    // 添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while(true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        // 在阻塞的这段时间里，进程可能会收到内核的一些信号，这些信号优先级高，需要优先处理的，不能等这些调用完成后才处理信号。
        // 于是系统先去处理信号，然后强制这些函数以出错的形式返回，
        // 其错误码 errno 就是 EINTR，相应的错误描述为“Interrupted system call”。这整个过程就是系统中断。
        if ((number < 0) && (errno != EINTR)) {
            printf( "epoll failure\n" );
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            // 有客户端连接进来
            if( sockfd == listenfd ) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if (connfd < 0) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    // 目前连接数满了
                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                // 对方异常断开或者错误等事件
                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) {

                if(users[sockfd].read()) {
                    // 一次性把所有数据都读完
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {
                // 一次性写完所有数据
                if( !users[sockfd].write() ) { 
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}