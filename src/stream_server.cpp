/**
 * Copyright : falcon build system (c) 2014.
 * LICENSE : see accompanying LICENSE file for details.
 */

#include <algorithm>
#include <cassert>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "stream_server.h"

#include "util/event.h"
#include "exceptions.h"
#include "graph_builder.h"
#include "logging.h"
#include "posix_subprocess.h"

namespace falcon {

StreamServer::StreamServer()
  : serverSocket_(-1)
  , eventFd_()
  , stopped_(false) { }

void StreamServer::openPort(unsigned int port) {
  serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket_ < 0) {
    THROW_ERROR(errno, "Failed to open socket");
  }

  int on = 1;
  if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &on,
                 sizeof(on)) < 0) {
    close(serverSocket_);
    THROW_ERROR(errno, "setsockopt");
  }

  if (ioctl(serverSocket_, FIONBIO, &on) < 0) {
    close(serverSocket_);
    THROW_ERROR(errno, "ioctl");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(serverSocket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(serverSocket_);
    THROW_ERROR(errno, "bind");
  }

  if (listen(serverSocket_, 32) < 0) {
    close(serverSocket_);
    THROW_ERROR(errno, "listen");
  }
}

StreamServer::~StreamServer() {
  std::lock_guard<std::mutex> lock(mutex_);

  /* Make sure all sockets are closed. */
  for (auto it = map_.begin(); it != map_.end();) {
    auto next = std::next(it);
    closeClient(*it->second.itFd);
    it = next;
  }

  /* Only the server socket should remain in fds_. Close it. */
  close(serverSocket_);
}

void StreamServer::run() {
  while (!stopped_) {
    processEvents();
  }
}

void StreamServer::stop() {
  stopped_ = true;
  notifyPoll();
}

/* Locking is performed by the callers (closeClient, endBuild). */
void StreamServer::removeBuild(std::list<BuildInfo>::iterator it) {
  assert(it->refcount == 0);
  assert(it->buildCompleted);
  builds_.erase(it);
}

void StreamServer::newBuild(unsigned int buildId) {
  std::lock_guard<std::mutex> lock(mutex_);

  /* The previous build might be ready for removal if there are no more clients
   * reading its output. */
  if (!builds_.empty()) {
    /* endBuild should have been called prior to calling newBuild. */
    assert(builds_.front().buildCompleted);
    if (builds_.front().refcount == 0) {
      removeBuild(builds_.begin());
    }
  }

  auto info = BuildInfo(buildId);
  builds_.push_front(std::move(info));

  writeBuf("{\n"
           "  \"id\": ");
  std::ostringstream ss;
  ss << buildId;
  writeBuf(ss.str());
  writeBuf(",\n"
           "  \"cmds\": [\n");
  flushWaiting();
}

void StreamServer::endBuild(BuildResult result) {
  std::lock_guard<std::mutex> lock(mutex_);

  /* There should be an ongoing build. */
  assert(!builds_.empty() && !builds_.front().buildCompleted);

  writeBuf("\n"
           "  ],\n"
           "  \"result\": \"");
  writeBuf(toString(result));
  writeBuf("\"\n"
           "}\n");
  flushWaiting();

  builds_.front().buildCompleted = true;
}

void StreamServer::processEvents() {
  std::vector<pollfd> fds;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::for_each(fds_.begin(), fds_.end(),
        [&fds](const int& fd) {
          fds.push_back({ fd, POLLOUT });
        }
    );

    fds.push_back({ serverSocket_, POLLIN });
    fds.push_back({ eventFd_.get(), POLLIN });
  }

  int r = poll(&fds.front(), fds.size(), -1);
  if (r < 0) {
    if (errno == EINTR) {
      return;
    } else if (errno == EINVAL) {
      /* Too many file descriptors. */
      LOG(ERROR) << "Too many fds for poll";
    }
    THROW_ERROR(errno, "poll failed");
  } else if (r == 0) {
    /* Timeout. */
    return;
  }

  /* Try to read data from each fd that is ready. */
  for (auto it = fds.begin(); it != fds.end(); ++it) {
    if (it->revents == 0) {
      continue;
    }
    if (it->fd == serverSocket_) {
      if (it->revents & POLLIN) {
        acceptClients();
      } else {
        LOG(ERROR) << "Unexpected poll event " << it->revents;
      }
    } else if (it->fd != eventFd_.get()) {
      if (it->revents & POLLOUT) {
        processClient(it->fd);
      } else {
        LOG(ERROR) << "Unexpected poll event " << it->revents;
      }
    } else {
      /* Notified by eventfd. */
      flushEventFd();
    }
  }
}

void StreamServer::flushEventFd() {
  eventFd_.flush();
}

void StreamServer::acceptClients() {
  /* Accept each incoming connection. */
  int clientFd = -1;
  do {
    clientFd = accept(serverSocket_, NULL, NULL);
    if (clientFd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* We accepted all the connections. */
        break;
      } else {
        THROW_ERROR(errno, "accept");
      }
    } else {
      createClient(clientFd);
    }
  } while (clientFd != -1);
}

void StreamServer::createClient(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);

  bool isWaiting = builds_.empty()
    || builds_.front().buf.size() == 0;

  std::list<int>::iterator itFd;
  if (isWaiting) {
    waiting_.push_front(fd);
    itFd = waiting_.begin();
  } else {
    fds_.push_front(fd);
    itFd = fds_.begin();
  }

  if (!builds_.empty()) {
    builds_.front().refcount++;
  }

  map_[fd] = ClientInfo{ builds_.begin(), 0, itFd, isWaiting};
}

void StreamServer::processClient(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(fd);
  assert(it != map_.end());
  ClientInfo& info = it->second;
  auto itBuild = info.itBuild;

  /* There should be a build and some data to be read. Otherwise this fd should
   * be in the waiting list. */
  assert(itBuild != builds_.end());
  assert(info.bufPtr < itBuild->buf.size());

  std::string& buf = itBuild->buf;
  std::size_t bufSize = buf.size() - info.bufPtr;
  const char* bufPtr = &buf[info.bufPtr];
  do {
#ifndef MSG_NOSIGNAL
    #warning "MSG_NOSIGNAL is not defined in your environment: this may have unexpected side effect"
    int r = send(fd, bufPtr, bufSize, 0);
#else
    int r = send(fd, bufPtr, bufSize, MSG_NOSIGNAL);
#endif
    if (r < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        closeClient(fd);
      }
      return;
    }
    info.bufPtr += r;
    bufSize -= r;
  } while (info.bufPtr < buf.size());

  /* If we reach here, it means there is nothing left to write for this
   * client. */

  if (itBuild->buildCompleted) {
    closeClient(fd);
  } else {
    /* There might be more data. Put it in the waiting list. */
    fds_.erase(it->second.itFd);
    waiting_.push_front(fd);
    it->second.isWaiting = true;
    it->second.itFd = waiting_.begin();
  }
}

/* Locking already performed by processClient (the caller). */
void StreamServer::closeClient(int fd) {
  auto itMap = map_.find(fd);
  assert(itMap != map_.end());

  /* Decrement the refcount of the build. */
  auto itBuild = itMap->second.itBuild;
  assert(itBuild != builds_.end());
  itBuild->refcount--;

  /* Remove the build info if the refcount reaches 0, the build completed, and
   * we have a more recent build. The last check is here in order to make sure
   * that we always have at least one build in the list, so that when a new
   * client connects it is always assigned to a build. */
  if (itBuild->refcount == 0 && itBuild->buildCompleted
      && itBuild != builds_.begin()) {
    removeBuild(itBuild);
  }

  fds_.erase(itMap->second.itFd);
  map_.erase(itMap);
  close(fd);
}

void StreamServer::flushWaiting() {
  /* If we are flushing the waiting list, it means there is some new data
   * and thus we should have an ongoing build. */
  assert(!builds_.empty()
      && !builds_.front().buildCompleted
      && !builds_.front().buf.empty());

  for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
    /* Move the client fd from waiting_ to fds_. */
    fds_.push_front(*it);
    auto itMap = map_.find(*it);
    assert(itMap != map_.end());
    itMap->second.itFd = fds_.begin();
    itMap->second.isWaiting = false;

    /* Assign the client to the current build, if needed. */
    if (itMap->second.itBuild == builds_.end()) {
      itMap->second.itBuild = builds_.begin();
      itMap->second.itBuild->refcount++;
    } else {
      /* If the client in the waiting list was already assigned to a build, it
       * should be the current build, because we can only do a build at a time,
       * and any client that was reading the data of a previous build should
       * have been closed. */
      assert(itMap->second.itBuild == builds_.begin());
    }
  }
  waiting_.clear();

  notifyPoll();
}

void StreamServer::notifyPoll() {
  if (eventFd_.raise() != 0) {
    THROW_ERROR(errno, "raise event failed");
  }
}

void StreamServer::writeBuf(const std::string& str) {
  assert(!builds_.empty());
  builds_.front().buf.append(str);
}

void StreamServer::writeBufEscapeJson(const char* buf, std::size_t len) {
  assert(!builds_.empty());

  for (std::size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c == '"' || c == '\\') {
      builds_.front().buf.push_back('\\');
      builds_.front().buf.push_back(c);
    } else if (c == '\n') {
      builds_.front().buf.append("\\n");
    } else {
      builds_.front().buf.push_back(c);
    }
  }
}

void StreamServer::writeCmdOutput(unsigned int cmdId, char* buf,
                                  std::size_t len, bool isStdout) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(!builds_.empty());
  assert(!builds_.front().firstChunk);

  writeBuf(",\n");
  writeBuf("    { \"id\": ");
  std::ostringstream ss;
  ss << cmdId;
  writeBuf(ss.str());
  if (isStdout) {
    writeBuf(", \"stdout\": \"");
  } else {
    writeBuf(", \"stderr\": \"");
  }
  writeBufEscapeJson(buf, len);
  writeBuf("\" }");

  flushWaiting();
}

void StreamServer::writeStdout(unsigned int cmdId, char* buf, std::size_t len) {
  writeCmdOutput(cmdId, buf, len, true);
}

void StreamServer::writeStderr(unsigned int cmdId, char* buf, std::size_t len) {
  writeCmdOutput(cmdId, buf, len, false);
}

void StreamServer::newCommand(unsigned int cmdId, const std::string& cmd) {
  std::lock_guard<std::mutex> lock(mutex_);

  assert(!builds_.empty());

  if (builds_.front().firstChunk) {
    builds_.front().firstChunk = false;
  } else {
    writeBuf(",\n");
  }

  writeBuf("    { \"id\": ");
  std::ostringstream ss;
  ss << cmdId;
  writeBuf(ss.str());
  writeBuf(", \"cmd\": \"");
  writeBufEscapeJson(&cmd[0], cmd.size());
  writeBuf("\" }");

  flushWaiting();
}

void StreamServer::endCommand(unsigned int cmdId, SubProcessExitStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);

  assert(!builds_.empty());

  writeBuf(",\n");
  writeBuf("    { \"id\": ");
  std::ostringstream ss;
  ss << cmdId;
  writeBuf(ss.str());
  writeBuf(", \"status\": \"");
  writeBuf(toString(status));
  writeBuf("\" }");

  flushWaiting();
}

void StreamServer::cacheRetrieveAction(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  assert(!builds_.empty());

  if (builds_.front().firstChunk) {
    builds_.front().firstChunk = false;
  } else {
    writeBuf(",\n");
  }

  writeBuf("    { \"cache\": \"");
  std::ostringstream ss;
  ss << path;
  writeBuf(ss.str());
  writeBuf("\" }");

  flushWaiting();
}

} // namespace falcon
