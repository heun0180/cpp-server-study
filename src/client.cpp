#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <poll.h>
#include "protocol.h"

bool readInputLine(
    std::string& line,
    std::string& inputBuffer,
    const std::atomic<bool>& connected
)
{
    while (connected.load())
    {
        std::size_t newline = inputBuffer.find('\n');
        if (newline != std::string::npos)
        {
            line = inputBuffer.substr(0, newline);
            inputBuffer.erase(0, newline + 1);
            return true;
        }

        // 콘솔 입력이 없어도 수신 스레드의 종료 상태를 확인한다.
        pollfd input{STDIN_FILENO, POLLIN, 0};
        int result = poll(&input, 1, 100);
        if (result == -1 && errno == EINTR)
            continue;
        if (result == -1)
        {
            std::cout << "Input poll failed: " << std::strerror(errno) << std::endl;
            return false;
        }
        if (result == 0)
            continue;

        char buffer[1024];
        ssize_t receivedBytes = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (receivedBytes == -1 && errno == EINTR)
            continue;
        if (receivedBytes == -1)
        {
            std::cout << "Input read failed: " << std::strerror(errno) << std::endl;
            return false;
        }
        if (receivedBytes == 0)
        {
            line.swap(inputBuffer);
            return !line.empty() && connected.load();
        }

        inputBuffer.append(buffer, static_cast<std::size_t>(receivedBytes));
    }

    return false;
}

void receiveMessages(int clientSocket, std::atomic<bool>& connected)
{
    while (true)
    {
        std::string message;

        protocol::IoResult result = protocol::receiveMessage(clientSocket, message);
        if (!result)
        {
            bool wasConnected = connected.exchange(false);
            shutdown(clientSocket, SHUT_WR);
            if (wasConnected)
            {
                const char* reason = result.status == protocol::IoStatus::Closed
                    ? "Server disconnected: " : "Receive failed: ";
                std::cout << reason << protocol::resultMessage(result) << std::endl;
            }
            break;
        }

        std::cout << "Client: " << message << std::endl;
    }
}

int main()
{
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (clientSocket == -1)
    {
        std::cout << "Socket creation failed" << std::endl;
        return 1;
    }

    protocol::IoResult sendResult = protocol::configureSocket(clientSocket);
    if (!sendResult)
    {
        std::cout << "Socket setup failed: " << protocol::resultMessage(sendResult) << std::endl;
        close(clientSocket);
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(7777);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    //서버 연결 요청
    int result = connect(
        clientSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)
    );

    if (result == -1)
    {
        std::cout << "Connect failed: " << std::strerror(errno) << std::endl;
        close(clientSocket);
        return 1;
    }

    std::atomic<bool> connected{true};
    std::string inputBuffer;

    //닉네임 설정
    std::string nickname;
    std::cout << "Nickname: " << std::flush;
    if (!readInputLine(nickname, inputBuffer, connected) || nickname.empty() ||
        nickname.size() > protocol::maxNicknameSize)
    {
        std::cout << "Invalid nickname length (1-99 bytes)" << std::endl;
        close(clientSocket);
        return 1;
    }

    sendResult = protocol::sendMessage(clientSocket, nickname);
    if (!sendResult)
    {
        std::cout << "Nickname send failed: " << protocol::resultMessage(sendResult) << std::endl;
        close(clientSocket);
        return 1;
    }


    //방 번호 설정
    std::string roomId;
    std::cout << "Room ID: " << std::flush;

    if (!readInputLine(roomId, inputBuffer, connected) || roomId.empty() ||
        roomId.size() > protocol::maxRoomIdSize)
    {
        std::cout << "Invalid room ID length (1-99 bytes)" << std::endl;
        close(clientSocket);
        return 1;
    }

    sendResult = protocol::sendMessage(clientSocket, roomId);
    if (!sendResult)
    {
        std::cout << "Room ID send failed: " << protocol::resultMessage(sendResult) << std::endl;
        close(clientSocket);
        return 1;
    }

    std::thread receiveThread(receiveMessages, clientSocket, std::ref(connected));
    int exitCode = 0;

    while (connected.load())
    {
        std::string message;

        std::cout << "Message: " << std::flush;
        if (!readInputLine(message, inputBuffer, connected) || !connected.load())
            break;

        if (message.empty())
            continue;

        if (message.size() > protocol::maxMessageSize)
        {
            std::cout << "Message too long (max 4096 bytes)" << std::endl;
            continue;
        }

        sendResult = protocol::sendMessage(clientSocket, message);
        if (!sendResult)
        {
            std::cout << "Message send failed: " << protocol::resultMessage(sendResult) << std::endl;
            exitCode = 1;
            break;
        }
    }

    connected.store(false);
    shutdown(clientSocket, SHUT_RDWR);
    receiveThread.join();

    // 수신 스레드가 소켓을 사용하므로 join 이후에만 닫는다.
    close(clientSocket);

    return exitCode;
}
