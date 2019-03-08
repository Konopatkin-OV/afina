#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/logger.h>

#include <afina/Storage.h>
#include <afina/execute/Command.h>
#include <afina/logging/Service.h>

#include "protocol/Parser.h"

namespace Afina {
namespace Network {
namespace MTblocking {

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Logging::Service> pl) : Server(ps, pl) {}

// See Server.h
ServerImpl::~ServerImpl() {}

// See Server.h
void ServerImpl::Start(uint16_t port, uint32_t n_accept, uint32_t n_workers) {
    _logger = pLogging->select("network");
    _logger->info("Start mt_blocking network service");

    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(port);       // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any address

    _server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_socket == -1) {
        throw std::runtime_error("Failed to open socket");
    }

    int opts = 1;
    if (setsockopt(_server_socket, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(opts)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket setsockopt() failed");
    }

    if (bind(_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket bind() failed");
    }

    if (listen(_server_socket, 5) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket listen() failed");
    }

    running.store(true);
    _num_workers = 0;
    _thread = std::thread(&ServerImpl::OnRun, this);
}

// See Server.h
void ServerImpl::Stop() {
    running.store(false);

    //waiting for all workers to finish
    std::unique_lock<std::mutex> _lock(_work_mutex);
    while (_num_workers > 0) {
        _all_finished.wait(_lock);
    }

    shutdown(_server_socket, SHUT_RDWR);
}

// See Server.h
void ServerImpl::Join() {
    assert(_thread.joinable());
    _thread.join();
    close(_server_socket);
}

// See Server.h
void ServerImpl::OnRun() {
    // Here is connection state
    // - parser: parse state of the stream
    // - command_to_execute: last command parsed out of stream
    // - arg_remains: how many bytes to read from stream to get command argument
    // - argument_for_command: buffer stores argument
    std::size_t arg_remains;

    std::string argument_for_command;
    std::unique_ptr<Execute::Command> command_to_execute;
    while (running.load()) {
        _logger->debug("waiting for connection...");

        // The call to accept() blocks until the incoming connection arrives
        int client_socket;
        struct sockaddr client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        if ((client_socket = accept(_server_socket, (struct sockaddr *)&client_addr, &client_addr_len)) == -1) {
            continue;
        }

        // Got new connection
        if (_logger->should_log(spdlog::level::debug)) {
            std::string host = "unknown", port = "-1";

            char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
            if (getnameinfo(&client_addr, client_addr_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                host = hbuf;
                port = sbuf;
            }
            _logger->debug("Accepted connection on descriptor {} (host={}, port={})\n", client_socket, host, port);
        }

        // Configure read timeout
        {
            struct timeval tv;
            tv.tv_sec = 5; // TODO: make it configurable
            tv.tv_usec = 0;
            setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        }

        // Start new thread and process data from/to connection
        {
            {
                std::unique_lock<std::mutex> _lock(_work_mutex);
                _num_workers += 1;
            }
            std::thread worker_thread(&ServerImpl::OnCommand, this, client_socket);
            worker_thread.detach();
        }
    }

    // Cleanup on exit...
    _logger->warn("Network stopped");
}

void ServerImpl::OnCommand (int client_socket) {
    const size_t buf_size = 1024;
    char buf[buf_size];
    size_t buf_read, buf_left = 0, buf_parsed;

    Protocol::Parser parser;
    parser.Reset();
    bool parsed = false;

    // reading from socket until command is parsed
    while (!parsed) {
        buf_read = recv(client_socket, buf + buf_left, buf_size - buf_left, 0);
        buf_left += buf_read;
        parsed = parser.Parse(buf, buf_left, buf_parsed);

        if (buf_parsed > 0) {
            strncpy(buf, buf + buf_parsed, buf_left - buf_parsed);
            buf_left -= buf_parsed;
        }
    }

    std::unique_ptr<Execute::Command> command(parser.Build(buf_parsed));

    // check if command args fit into buffer
    if (buf_parsed + buf_left + 1 <= buf_size) {
        // reading command args from socket
        while (buf_left < buf_parsed) {
            buf_left += recv(client_socket, buf + buf_left, buf_parsed - buf_left, 0);
        }
        buf[buf_left] = '\0';

        std::string msg;
        command->Execute(*pStorage, buf, msg);
        msg.append("\r\n");

        if (send(client_socket, msg.data(), msg.size(), 0) <= 0) {
            _logger->error("Failed to write response to client: {}", strerror(errno));
        }
    } else {
        static const std::string msg = "Command arguments are too long";
        if (send(client_socket, msg.data(), msg.size(), 0) <= 0) {
            _logger->error("Failed to write response to client: {}", strerror(errno));
        }
    }

    close(client_socket);

    // for debugging
    sleep(5);
    _logger->debug("Work finished");

    {
        std::unique_lock<std::mutex> _lock(_work_mutex);
        _num_workers -= 1;
        _all_finished.notify_one();
    }
}

} // namespace MTblocking
} // namespace Network
} // namespace Afina
