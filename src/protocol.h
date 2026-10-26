#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

namespace protocol
{
constexpr std::size_t maxNicknameSize = 99;
constexpr std::size_t maxRoomIdSize = 99;
constexpr std::size_t maxMessageSize = 4096;
// 서버가 붙이는 "[닉네임] " 접두어까지 포함한다.
constexpr std::size_t maxFrameSize = maxMessageSize + maxNicknameSize + 3;

inline bool sendAll(int socket, const char* data, std::size_t size)
{
    std::size_t sent = 0;

    while (sent < size)
    {
        ssize_t sentBytes = send(socket, data + sent, size - sent, MSG_NOSIGNAL);

        if (sentBytes == -1 && errno == EINTR)
            continue;

        if (sentBytes <= 0)
            return false;

        sent += static_cast<std::size_t>(sentBytes);
    }

    return true;
}

inline bool receiveAll(int socket, char* data, std::size_t size)
{
    std::size_t received = 0;

    while (received < size)
    {
        ssize_t receivedBytes = recv(socket, data + received, size - received, 0);

        if (receivedBytes == -1 && errno == EINTR)
            continue;

        if (receivedBytes <= 0)
            return false;

        received += static_cast<std::size_t>(receivedBytes);
    }

    return true;
}

// 같은 소켓에 대한 송신은 호출하는 쪽에서 직렬화한다.
inline bool sendMessage(int socket, const std::string& message)
{
    if (message.empty() || message.size() > maxFrameSize)
        return false;

    std::uint32_t length = htonl(static_cast<std::uint32_t>(message.size()));

    return sendAll(socket, reinterpret_cast<const char*>(&length), sizeof(length)) &&
        sendAll(socket, message.data(), message.size());
}

inline bool receiveMessage(
    int socket,
    std::string& message,
    std::size_t maxSize = maxFrameSize
)
{
    std::uint32_t length = 0;

    if (!receiveAll(socket, reinterpret_cast<char*>(&length), sizeof(length)))
        return false;

    length = ntohl(length);

    // 길이를 먼저 검증해 잘못된 헤더로 큰 메모리가 할당되지 않도록 한다.
    if (length == 0 || length > maxSize || length > maxFrameSize)
        return false;

    message.resize(length);

    return receiveAll(socket, message.data(), message.size());
}
}
