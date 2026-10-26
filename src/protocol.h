#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    IncompleteFrame
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

inline IoResult receiveAll(int socket, char* data, std::size_t size)
{
    std::size_t received = 0;

    while (received < size)
    {
        ssize_t receivedBytes = recv(socket, data + received, size - received, 0);

        if (receivedBytes == -1 && errno == EINTR)
            continue;

        if (receivedBytes == -1)
            return {IoStatus::Error, errno};

        if (receivedBytes == 0)
            return {received == 0 ? IoStatus::Closed : IoStatus::IncompleteFrame};

        received += static_cast<std::size_t>(receivedBytes);
    }

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
    std::size_t maxSize = maxFrameSize
)
{
    std::uint32_t length = 0;

    IoResult result = receiveAll(socket, reinterpret_cast<char*>(&length), sizeof(length));
    if (!result)
        return result;

    length = ntohl(length);

    // 길이를 먼저 검증해 잘못된 헤더로 큰 메모리가 할당되지 않도록 한다.
    if (length == 0 || length > maxSize || length > maxFrameSize)
        return {IoStatus::InvalidLength};

    message.resize(length);

    result = receiveAll(socket, message.data(), message.size());
    if (result.status == IoStatus::Closed)
        result.status = IoStatus::IncompleteFrame;

    return result;
}
}
