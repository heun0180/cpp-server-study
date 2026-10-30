#include <sys/socket.h>
#ifdef TEST_SO_NOSIGPIPE
#undef MSG_NOSIGNAL
#endif
#include "protocol.h"
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>

volatile std::sig_atomic_t signalCount = 0;

void handleSignal(int)
{
    ++signalCount;
}

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void createSocketPair(int (&sockets)[2])
{
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair");
    require(static_cast<bool>(protocol::configureSocket(sockets[0])), "configure sender");
    require(static_cast<bool>(protocol::configureSocket(sockets[1])), "configure receiver");
}

void testReceiveStatus()
{
    for (int kind = 0; kind < 4; ++kind)
    {
        int sockets[2];
        createSocketPair(sockets);
        if (kind == 1)
        {
            require(static_cast<bool>(protocol::sendAll(sockets[1], "\0\0", 2)), "partial header");
        }
        else if (kind >= 2)
        {
            std::uint32_t length = htonl(10);
            require(static_cast<bool>(protocol::sendAll(
                sockets[1], reinterpret_cast<const char*>(&length), sizeof(length)
            )), "body header");
            if (kind == 3)
                require(static_cast<bool>(protocol::sendAll(sockets[1], "short", 5)), "partial body");
        }
        close(sockets[1]);

        std::string message;
        protocol::IoResult result = protocol::receiveMessage(sockets[0], message);
        protocol::IoStatus expected = kind == 0
            ? protocol::IoStatus::Closed : protocol::IoStatus::IncompleteFrame;
        require(result.status == expected && result.errorCode == 0, "EOF classification");
        close(sockets[0]);
    }

    std::string message;
    protocol::IoResult result = protocol::receiveMessage(-1, message);
    require(result.status == protocol::IoStatus::Error && result.errorCode == EBADF, "recv errno");

    int sockets[2];
    createSocketPair(sockets);
    int flags = fcntl(sockets[0], F_GETFL);
    require(flags != -1 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking recv");
    result = protocol::receiveMessage(sockets[0], message);
    require(result.status == protocol::IoStatus::Error &&
        (result.errorCode == EAGAIN || result.errorCode == EWOULDBLOCK), "recv EAGAIN must return");
    close(sockets[0]);
    close(sockets[1]);
    std::cout << "PASS: recv EOF, incomplete frame, EBADF, EAGAIN" << std::endl;
}

void testBrokenSend()
{
    int sockets[2];
    createSocketPair(sockets);
    close(sockets[1]);
    protocol::IoResult result = protocol::sendMessage(sockets[0], "after close");
    require(result.status == protocol::IoStatus::Error &&
        (result.errorCode == EPIPE || result.errorCode == ECONNRESET), "send after close errno");
    close(sockets[0]);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    require(listener != -1, "TCP listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "TCP bind");
    require(listen(listener, 1) == 0, "TCP listen");
    socklen_t addressSize = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0, "TCP port");
    sockets[0] = socket(AF_INET, SOCK_STREAM, 0);
    require(sockets[0] != -1, "TCP socket");
    require(connect(sockets[0], reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "TCP connect");
    sockets[1] = accept(listener, nullptr, nullptr);
    require(sockets[1] != -1, "TCP accept");
    close(listener);
    require(static_cast<bool>(protocol::configureSocket(sockets[0])), "configure TCP sender");

    linger reset{1, 0};
    require(setsockopt(sockets[1], SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0, "TCP reset option");
    close(sockets[1]);
    char byte;
    result = protocol::receiveAll(sockets[0], &byte, 1);
    require(result.status == protocol::IoStatus::Error && result.errorCode == ECONNRESET, "TCP reset recv");
    result = protocol::sendMessage(sockets[0], "after TCP reset");
    require(result.status == protocol::IoStatus::Error &&
        (result.errorCode == EPIPE || result.errorCode == ECONNRESET || result.errorCode == ENOTCONN),
        "send after TCP reset errno");
    close(sockets[0]);
    std::cout << "PASS: socketpair/TCP send after close without SIGPIPE termination" << std::endl;
}

void testInterruptedReceive()
{
    int sockets[2];
    createSocketPair(sockets);
    pthread_t receiver = pthread_self();
    signalCount = 0;
    std::thread sender([&]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(pthread_kill(receiver, SIGUSR1) == 0, "interrupt recv");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(static_cast<bool>(protocol::sendMessage(sockets[1], "after EINTR")), "send after signal");
    });

    std::string message;
    protocol::IoResult result = protocol::receiveMessage(sockets[0], message);
    sender.join();
    require(signalCount > 0 && static_cast<bool>(result) && message == "after EINTR", "recv EINTR retry");
    close(sockets[0]);
    close(sockets[1]);
    std::cout << "PASS: recv during signal delivery" << std::endl;
}

void testInterruptedSend()
{
    int sockets[2];
    createSocketPair(sockets);
    int bufferSize = 1024;
    require(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) == 0, "send buffer");
    int flags = fcntl(sockets[0], F_GETFL);
    require(flags != -1 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking send");

    char padding = 'p';
    std::size_t paddingSize = 0;
    while (true)
    {
        ssize_t sent = send(sockets[0], &padding, 1, 0);
        if (sent == -1)
        {
            require(errno == EAGAIN || errno == EWOULDBLOCK, "fill send buffer");
            break;
        }
        require(sent > 0, "fill send buffer progress");
        paddingSize += static_cast<std::size_t>(sent);
    }
    protocol::IoResult result = protocol::sendAll(sockets[0], "x", 1);
    require(result.status == protocol::IoStatus::Error &&
        (result.errorCode == EAGAIN || result.errorCode == EWOULDBLOCK), "send EAGAIN must return");
    require(fcntl(sockets[0], F_SETFL, flags) == 0, "blocking send");

    std::string payload(256 * 1024, 'x');
    std::string received;
    pthread_t sendingThread = pthread_self();
    signalCount = 0;
    std::thread reader([&]()
    {
        // 버퍼가 가득 찬 송신을 중단한 뒤, 조금씩 읽으면서도 신호를 보낸다.
        for (int i = 0; i < 2; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(pthread_kill(sendingThread, SIGUSR1) == 0, "interrupt blocked send");
        }

        char buffer[512];
        int reads = 0;
        while (true)
        {
            ssize_t size = recv(sockets[1], buffer, sizeof(buffer), 0);
            if (size == -1 && errno == EINTR)
                continue;
            require(size >= 0, "drain send buffer");
            if (size == 0)
                break;
            received.append(buffer, static_cast<std::size_t>(size));
            if (++reads % 16 == 0)
                require(pthread_kill(sendingThread, SIGUSR1) == 0, "interrupt partial send");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    result = protocol::sendAll(sockets[0], payload.data(), payload.size());
    shutdown(sockets[0], SHUT_WR);
    reader.join();
    require(signalCount >= 2 && static_cast<bool>(result), "send EINTR retry");
    require(received == std::string(paddingSize, 'p') + payload, "partial send data integrity");
    close(sockets[0]);
    close(sockets[1]);
    std::cout << "PASS: full send buffer, EAGAIN, signals, 256 KiB integrity" << std::endl;
}

void testReceiveDeadline()
{
    int sockets[2];
    createSocketPair(sockets);
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(250);
    std::thread sender([&]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(static_cast<bool>(protocol::sendMessage(sockets[1], "nickname")), "deadline nickname");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        require(static_cast<bool>(protocol::sendMessage(sockets[1], "room")), "late room");
    });

    std::string message;
    protocol::IoResult result = protocol::receiveMessage(sockets[0], message, 99, deadline);
    require(static_cast<bool>(result) && message == "nickname", "nickname before deadline");
    result = protocol::receiveMessage(sockets[0], message, 99, deadline);
    auto elapsed = std::chrono::steady_clock::now() - start;
    require(result.status == protocol::IoStatus::Timeout, "shared deadline across frames");
    require(elapsed >= std::chrono::milliseconds(240) && elapsed < std::chrono::milliseconds(450),
        "deadline duration");
    sender.join();
    result = protocol::receiveMessage(sockets[0], message, 99, deadline);
    require(result.status == protocol::IoStatus::Timeout, "expired deadline with queued data");
    close(sockets[0]);
    close(sockets[1]);

    createSocketPair(sockets);
    pthread_t receiver = pthread_self();
    signalCount = 0;
    start = std::chrono::steady_clock::now();
    deadline = start + std::chrono::milliseconds(250);
    std::thread slowSender([&]()
    {
        std::uint32_t length = htonl(10);
        require(static_cast<bool>(protocol::sendAll(
            sockets[1], reinterpret_cast<const char*>(&length), sizeof(length)
        )), "slow body header");
        for (int i = 0; i < 8; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            require(pthread_kill(receiver, SIGUSR1) == 0, "interrupt deadline poll");
            require(static_cast<bool>(protocol::sendAll(sockets[1], "x", 1)), "slow body");
        }
        close(sockets[1]);
    });
    result = protocol::receiveMessage(sockets[0], message, 99, deadline);
    elapsed = std::chrono::steady_clock::now() - start;
    slowSender.join();
    require(result.status == protocol::IoStatus::Timeout && signalCount > 0, "progress and EINTR deadline");
    require(elapsed >= std::chrono::milliseconds(240) && elapsed < std::chrono::milliseconds(450),
        "progress must not extend deadline");
    close(sockets[0]);
    std::cout << "PASS: fixed deadline across frames, partial body, EINTR, expired queued data" << std::endl;
}

int main()
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGPIPE, &action, nullptr) == 0, "default SIGPIPE");
    action.sa_handler = handleSignal;
    require(sigaction(SIGUSR1, &action, nullptr) == 0, "signal handler without SA_RESTART");

    testReceiveStatus();
    testBrokenSend();
    testInterruptedReceive();
    testInterruptedSend();
    testReceiveDeadline();
}
