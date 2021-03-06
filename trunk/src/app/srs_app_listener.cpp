/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2017 OSSRS(winlin)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <srs_app_listener.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

#include <srs_kernel_log.hpp>
#include <srs_kernel_error.hpp>
#include <srs_app_server.hpp>
#include <srs_app_utility.hpp>

// set the max packet size.
#define SRS_UDP_MAX_PACKET_SIZE 65535

// sleep in ms for udp recv packet.
#define SrsUdpPacketRecvCycleMS 0

// nginx also set to 512
#define SERVER_LISTEN_BACKLOG 512

ISrsUdpHandler::ISrsUdpHandler()
{
}

ISrsUdpHandler::~ISrsUdpHandler()
{
}

srs_error_t ISrsUdpHandler::on_stfd_change(srs_netfd_t /*fd*/)
{
    return srs_success;
}

ISrsTcpHandler::ISrsTcpHandler()
{
}

ISrsTcpHandler::~ISrsTcpHandler()
{
}

SrsUdpListener::SrsUdpListener(ISrsUdpHandler* h, string i, int p)
{
    handler = h;
    ip = i;
    port = p;
    
    _fd = -1;
    _stfd = NULL;
    
    nb_buf = SRS_UDP_MAX_PACKET_SIZE;
    buf = new char[nb_buf];
    
    trd = new SrsDummyCoroutine();
}

SrsUdpListener::~SrsUdpListener()
{
    // close the stfd to trigger thread to interrupted.
    srs_close_stfd(_stfd);
    
    srs_freep(trd);
    
    // st does not close it sometimes,
    // close it manually.
    close(_fd);
    
    srs_freepa(buf);
}

int SrsUdpListener::fd()
{
    return _fd;
}

srs_netfd_t SrsUdpListener::stfd()
{
    return _stfd;
}

srs_error_t SrsUdpListener::listen()
{
    srs_error_t err = srs_success;
    
    if ((_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        return srs_error_new(ERROR_SOCKET_CREATE, "create socket");
    }
    
    srs_fd_close_exec(_fd);
    srs_socket_reuse_addr(_fd);
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (bind(_fd, (const sockaddr*)&addr, sizeof(sockaddr_in)) == -1) {
        return srs_error_new(ERROR_SOCKET_BIND, "bind socket");
    }
    
    if ((_stfd = srs_netfd_open_socket(_fd)) == NULL){
        return srs_error_new(ERROR_ST_OPEN_SOCKET, "st open socket");
    }
    
    srs_freep(trd);
    trd = new SrsSTCoroutine("udp", this);
    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "start thread");
    }
    
    return err;
}

srs_error_t SrsUdpListener::cycle()
{
    srs_error_t err = srs_success;
    
    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "udp listener");
        }
        
        // TODO: FIXME: support ipv6, @see man 7 ipv6
        sockaddr_in from;
        int nb_from = sizeof(sockaddr_in);
        int nread = 0;
        
        if ((nread = srs_recvfrom(_stfd, buf, nb_buf, (sockaddr*)&from, &nb_from, SRS_UTIME_NO_TIMEOUT)) <= 0) {
            return srs_error_new(ERROR_SOCKET_READ, "udp read, nread=%d", nread);
        }
        
        if ((err = handler->on_udp_packet(&from, buf, nread)) != srs_success) {
            return srs_error_wrap(err, "handle packet %d bytes", nread);
        }
        
        if (SrsUdpPacketRecvCycleMS > 0) {
            srs_usleep(SrsUdpPacketRecvCycleMS * 1000);
        }
    }
    
    return err;
}

SrsTcpListener::SrsTcpListener(ISrsTcpHandler* h, string i, int p)
{
    handler = h;
    ip = i;
    port = p;
    
    _fd = -1;
    _stfd = NULL;
    
    trd = new SrsDummyCoroutine();
}

SrsTcpListener::~SrsTcpListener()
{
    srs_freep(trd);
    
    srs_close_stfd(_stfd);
}

int SrsTcpListener::fd()
{
    return _fd;
}

srs_error_t SrsTcpListener::listen()
{
    srs_error_t err = srs_success;
    
    if ((_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        return srs_error_new(ERROR_SOCKET_CREATE, "create socket");
    }
    
    srs_fd_close_exec(_fd);
    srs_socket_reuse_addr(_fd);
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (bind(_fd, (const sockaddr*)&addr, sizeof(sockaddr_in)) == -1) {
        return srs_error_new(ERROR_SOCKET_BIND, "bind socket");
    }
    
    if (::listen(_fd, SERVER_LISTEN_BACKLOG) == -1) {
        return srs_error_new(ERROR_SOCKET_LISTEN, "listen socket");
    }
    
    if ((_stfd = srs_netfd_open_socket(_fd)) == NULL){
        return srs_error_new(ERROR_ST_OPEN_SOCKET, "st open socket");
    }
    
    srs_freep(trd);
    trd = new SrsSTCoroutine("tcp", this);
    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "start coroutine");
    }
    
    return err;
}

srs_error_t SrsTcpListener::cycle()
{
    srs_error_t err = srs_success;
    
    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "tcp listener");
        }
        
        srs_netfd_t cstfd = srs_accept(_stfd, NULL, NULL, SRS_UTIME_NO_TIMEOUT);
        if(cstfd == NULL){
            return srs_error_new(ERROR_SOCKET_CREATE, "accept failed");
        }
        
        int cfd = srs_netfd_fileno(cstfd);
        srs_fd_close_exec(cfd);
        
        if ((err = handler->on_tcp_client(cstfd)) != srs_success) {
            return srs_error_wrap(err, "handle fd=%d", cfd);
        }
    }
    
    return err;
}

