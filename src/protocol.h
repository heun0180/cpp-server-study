#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <poll.h>
#include <limits>
#include <algorithm>
#include <string>

namespace protocol
{
constexpr std::size_t maxNicknameSize = 99;
constexpr std::size_t maxRoomIdSize = 99;
constexpr std::size_t maxMessageSize = 4096;
// 서버가 붙이는 "[닉네임] " 접두어까지 포함한다.
constexpr std::size_t maxFrameSize = maxMessageSize + maxNicknameSize + 3;

enum class IoStatus
{
    Success,
    Closed,
    Error,
    InvalidLength,
    IncompleteFrame,
    Timeout
};

struct IoResult
{
    IoStatus status;
    int errorCode = 0;

    explicit operator bool() const
    {
        return status == IoStatus::Success;
    }
};

inline const char* resultMessage(const IoResult& result)
{
    switch (result.status)
    {
    case IoStatus::Success:
        return "Success";
    case IoStatus::Closed:
        return "Peer closed connection";
    case IoStatus::Error:
        return std::strerror(result.errorCode);
    case IoStatus::InvalidLength:
        return "Invalid frame length";
    case IoStatus::IncompleteFrame:
        return "Connection closed during frame";
    case IoStatus::Timeout:
        return "Receive deadline exceeded";
    }

    return "Unknown I/O status";
}

inline IoResult configureSocket(int socket)
{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    // MSG_NOSIGNAL이 없는 macOS에서는 소켓 옵션으로 SIGPIPE를 막는다.
    int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == -1)
        return {IoStatus::Error, errno};
#else
    (void)socket;
#endif
    return {IoStatus::Success};
}

inline IoResult sendAll(int socket, const char* data, std::size_t size)
{
#ifdef MSG_NOSIGNAL
    constexpr int sendFlags = MSG_NOSIGNAL;
#else
    constexpr int sendFlags = 0;
#endif

    std::size_t sent = 0;

    while (sent < size)
    {
        ssize_t sentBytes = send(socket, data + sent, size - sent, sendFlags);

        if (sentBytes == -1 && errno == EINTR)
            continue;

        if (sentBytes == -1)
            return {IoStatus::Error, errno};

        if (sentBytes == 0)
            return {IoStatus::Error, EIO};

        sent += static_cast<std::size_t>(sentBytes);
    }

    return {IoStatus::Success};
}

inline IoResult receiveAll(
    int socket,
    char* data,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
)
{
    std::size_t received = 0;
    bool hasDeadline = deadline != std::chrono::steady_clock::time_point::max();

    while (received < size)
    {
        if (hasDeadline)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return {IoStatus::Timeout};

            auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            int timeout = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
                remaining, std::numeric_limits<int>::max()
            ));
            pollfd input{socket, POLLIN, 0};
            int result = poll(&input, 1, timeout);
            if (result == -1 && errno == EINTR)
                continue;
            if (result == -1)
                return {IoStatus::Error, errno};
            if (result == 0)
                continue;
            if (std::chrono::steady_clock::now() >= deadline)
                return {IoStatus::Timeout};
        }

        // 제한 시간은 poll로 관리하고 recv 자체는 블로킹하지 않는다.
        int flags = hasDeadline ? MSG_DONTWAIT : 0;
        ssize_t receivedBytes = recv(socket, data + received, size - received, flags);

        if (receivedBytes == -1 && errno == EINTR)
            continue;

        if (receivedBytes == -1 && hasDeadline && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;

        if (receivedBytes == -1)
            return {IoStatus::Error, errno};

        if (receivedBytes == 0)
            return {received == 0 ? IoStatus::Closed : IoStatus::IncompleteFrame};

        received += static_cast<std::size_t>(receivedBytes);
    }

    if (hasDeadline && std::chrono::steady_clock::now() >= deadline)
        return {IoStatus::Timeout};

    return {IoStatus::Success};
}

// 같은 소켓에 대한 송신은 호출하는 쪽에서 직렬화한다.
inline IoResult sendMessage(int socket, const std::string& message)
{
    if (message.empty() || message.size() > maxFrameSize)
        return {IoStatus::InvalidLength};

    std::uint32_t length = htonl(static_cast<std::uint32_t>(message.size()));

    IoResult result = sendAll(socket, reinterpret_cast<const char*>(&length), sizeof(length));
    if (!result)
        return result;

    return sendAll(socket, message.data(), message.size());
}

inline IoResult receiveMessage(
    int socket,
    std::string& message,
    std::size_t maxSize = maxFrameSize,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
)
{
    std::uint32_t length = 0;

    IoResult result = receiveAll(socket, reinterpret_cast<char*>(&length), sizeof(length), deadline);
    if (!result)
        return result;

    length = ntohl(length);

    // 길이를 먼저 검증해 잘못된 헤더로 큰 메모리가 할당되지 않도록 한다.
    if (length == 0 || length > maxSize || length > maxFrameSize)
        return {IoStatus::InvalidLength};

    message.resize(length);

    result = receiveAll(socket, message.data(), message.size(), deadline);
    if (result.status == IoStatus::Closed)
        result.status = IoStatus::IncompleteFrame;

    return result;
}
}
