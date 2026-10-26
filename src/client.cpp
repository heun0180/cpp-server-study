#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include "protocol.h"

void receiveMessages(int clientSocket)
{
    while (true)
    {
        std::string message;

        if (!protocol::receiveMessage(clientSocket, message))
        {
            shutdown(clientSocket, SHUT_WR);
            std::cout << "Server disconnected" << std::endl;
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

    //닉네임 설정
    std::string nickname;
    std::cout << "Nickname: ";
    if (!std::getline(std::cin, nickname) || nickname.empty() ||
        nickname.size() > protocol::maxNicknameSize)
    {
        std::cout << "Invalid nickname length (1-99 bytes)" << std::endl;
        close(clientSocket);
        return 1;
    }

    if (!protocol::sendMessage(clientSocket, nickname))
    {
        std::cout << "Nickname send failed" << std::endl;
        close(clientSocket);
        return 1;
    }


    //방 번호 설정
    std::string roomId;
    std::cout << "Room ID: ";

    if (!std::getline(std::cin, roomId) || roomId.empty() ||
        roomId.size() > protocol::maxRoomIdSize)
    {
        std::cout << "Invalid room ID length (1-99 bytes)" << std::endl;
        close(clientSocket);
        return 1;
    }

    if (!protocol::sendMessage(clientSocket, roomId))
    {
        std::cout << "Room ID send failed" << std::endl;
        close(clientSocket);
        return 1;
    }

    std::thread receiveThread(receiveMessages, clientSocket);

    while (true)
    {
        std::string message;

        std::cout << "Message: ";
        if (!std::getline(std::cin, message))
            break;

        if (message.empty())
            continue;

        if (message.size() > protocol::maxMessageSize)
        {
            std::cout << "Message too long (max 4096 bytes)" << std::endl;
            continue;
        }

        if (!protocol::sendMessage(clientSocket, message))
        {
            std::cout << "Message send failed" << std::endl;
            break;
        }
    }

    shutdown(clientSocket, SHUT_RDWR);
    receiveThread.join();

    //사용한 소켓 종료
    close(clientSocket);

    return 0;
}
