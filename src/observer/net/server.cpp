/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021
//

#include "net/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <memory>

#include "common/ini_setting.h"
#include "common/io/io.h"
#include "common/lang/mutex.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "session/session_stage.h"
#include "net/communicator.h"
#include "session/session.h"
#include "net/thread_handler.h"

using namespace common;

ServerParam::ServerParam()
{
  listen_addr        = INADDR_ANY;
  max_connection_num = MAX_CONNECTION_NUM_DEFAULT;
  port               = PORT_DEFAULT;
}

Server::Server(ServerParam input_server_param) : server_param_(input_server_param) {}

Server::~Server()
{
  if (started_) {
    shutdown();
  }
}

int Server::set_non_block(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    LOG_INFO("Failed to get flags of fd :%d. ", fd);
    return -1;
  }

  flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (flags == -1) {
    LOG_INFO("Failed to set non-block flags of fd :%d. ", fd);
    return -1;
  }
  return 0;
}

void Server::accept(int fd, short ev, void *arg)
{
  Server            *instance = (Server *)arg;
  struct sockaddr_in addr;
  socklen_t          addrlen = sizeof(addr);

  int ret = 0;

  int client_fd = ::accept(fd, (struct sockaddr *)&addr, &addrlen);
  if (client_fd < 0) {
    LOG_ERROR("Failed to accept client's connection, %s", strerror(errno));
    return;
  }

  char ip_addr[24];
  if (inet_ntop(AF_INET, &addr.sin_addr, ip_addr, sizeof(ip_addr)) == nullptr) {
    LOG_ERROR("Failed to get ip address of client, %s", strerror(errno));
    ::close(client_fd);
    return;
  }
  std::stringstream address;
  address << ip_addr << ":" << addr.sin_port;
  std::string addr_str = address.str();

  ret = instance->set_non_block(client_fd);
  if (ret < 0) {
    LOG_ERROR("Failed to set socket of %s as non blocking, %s", addr_str.c_str(), strerror(errno));
    ::close(client_fd);
    return;
  }

  if (!instance->server_param_.use_unix_socket) {
    // unix socket不支持设置NODELAY
    int yes = 1;
    ret     = setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    if (ret < 0) {
      LOG_ERROR("Failed to set socket of %s option as : TCP_NODELAY %s\n", addr_str.c_str(), strerror(errno));
      ::close(client_fd);
      return;
    }
  }

  Communicator *communicator = instance->communicator_factory_.create(instance->server_param_.protocol);

  RC rc = communicator->init(client_fd, new Session(Session::default_session()), addr_str);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to init communicator. rc=%s", strrc(rc));
    delete communicator;
    return;
  }

  rc = instance->thread_handler_->new_connection(communicator);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to handle new connection. rc=%s", strrc(rc));
    delete communicator;
    return;
  }

  LOG_INFO("Accepted connection from %s\n", communicator->addr());
}

int Server::start()
{
  if (server_param_.use_std_io) {
    return start_stdin_server();
  } else if (server_param_.use_unix_socket) {
    return start_unix_socket_server();
  } else {
    return start_tcp_server();
  }
}

int Server::start_tcp_server()
{
  int                ret = 0;
  struct sockaddr_in sa;

  server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_ < 0) {
    LOG_ERROR("socket(): can not create server socket: %s.", strerror(errno));
    return -1;
  }

  int yes = 1;
  ret     = setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (ret < 0) {
    LOG_ERROR("Failed to set socket option of reuse address: %s.", strerror(errno));
    ::close(server_socket_);
    return -1;
  }

  ret = set_non_block(server_socket_);
  if (ret < 0) {
    LOG_ERROR("Failed to set socket option non-blocking:%s. ", strerror(errno));
    ::close(server_socket_);
    return -1;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(server_param_.port);
  sa.sin_addr.s_addr = htonl(server_param_.listen_addr);

  ret = ::bind(server_socket_, (struct sockaddr *)&sa, sizeof(sa));
  if (ret < 0) {
    LOG_ERROR("bind(): can not bind server socket, %s", strerror(errno));
    ::close(server_socket_);
    return -1;
  }

  ret = listen(server_socket_, server_param_.max_connection_num);
  if (ret < 0) {
    LOG_ERROR("listen(): can not listen server socket, %s", strerror(errno));
    ::close(server_socket_);
    return -1;
  }
  LOG_INFO("Listen on port %d", server_param_.port);

  started_ = true;
  LOG_INFO("Observer start success");
  return 0;
}

int Server::start_unix_socket_server()
{
  int ret        = 0;
  server_socket_ = socket(PF_UNIX, SOCK_STREAM, 0);
  if (server_socket_ < 0) {
    LOG_ERROR("socket(): can not create unix socket: %s.", strerror(errno));
    return -1;
  }

  ret = set_non_block(server_socket_);
  if (ret < 0) {
    LOG_ERROR("Failed to set socket option non-blocking:%s. ", strerror(errno));
    ::close(server_socket_);
    return -1;
  }

  unlink(server_param_.unix_socket_path.c_str());  /// 如果不删除源文件，可能会导致bind失败

  struct sockaddr_un sockaddr;
  memset(&sockaddr, 0, sizeof(sockaddr));
  sockaddr.sun_family = PF_UNIX;
  snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", server_param_.unix_socket_path.c_str());

  ret = ::bind(server_socket_, (struct sockaddr *)&sockaddr, sizeof(sockaddr));
  if (ret < 0) {
    LOG_ERROR("bind(): can not bind server socket(path=%s), %s", sockaddr.sun_path, strerror(errno));
    ::close(server_socket_);
    return -1;
  }

  ret = listen(server_socket_, server_param_.max_connection_num);
  if (ret < 0) {
    LOG_ERROR("listen(): can not listen server socket, %s", strerror(errno));
    ::close(server_socket_);
    return -1;
  }
  LOG_INFO("Listen on unix socket: %s", sockaddr.sun_path);

  started_ = true;
  LOG_INFO("Observer start success");
  return 0;
}

int Server::start_stdin_server()
{
  unique_ptr<Communicator> communicator = communicator_factory_.create(server_param_.protocol);

  RC rc = communicator->init(STDIN_FILENO, new Session(Session::default_session()), "stdin");
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to init cli communicator. rc=%s", strrc(rc));
    return -1;
  }

  started_ = true;

  SessionStage session_stage;
  while (started_) {
    SessionEvent *event = nullptr;

    rc = communicator->read_event(event);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to read event. rc=%s", strrc(rc));
      return -1;
    }

    if (event == nullptr) {
      break;
    }

    /// 在当前线程立即处理对应的事件
    session_stage.handle_request(event);
  }

  return 0;
}

int Server::serve()
{
  thread_handler_ = ThreadHandler::create(server_param_.thread_handling.c_str());
  if (thread_handler_ == nullptr) {
    LOG_ERROR("Failed to create thread handler: %s", server_param_.thread_handling.c_str());
    return -1;
  }

  int retval = start();
  if (retval == -1) {
    LOG_PANIC("Failed to start network");
    exit(-1);
  }

  if (!server_param_.use_std_io) {
    struct pollfd poll_fd;
    poll_fd.fd = server_socket_;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    while (started_) {
      int ret = poll(&poll_fd, 1, 500);
      if (ret < 0) {
        LOG_ERROR("poll error. fd = %d, ret = %d, error=%s", poll_fd.fd, ret, strerror(errno));
        break;
      } else if (0 == ret) {
        LOG_TRACE("poll timeout. fd = %d", poll_fd.fd);
        continue;
      }

      if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        LOG_ERROR("poll error. fd = %d, revents = %d", poll_fd.fd, poll_fd.revents);
        break;
      }

      accept(server_socket_, 0, this);
    }
  }

  started_ = false;
  LOG_INFO("Server quit");
  return 0;
}

void Server::shutdown()
{
  LOG_INFO("Server shutting down");

  // cleanup
  started_ = false;
}
