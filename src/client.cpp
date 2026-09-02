#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int main()
{
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(7777);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    //서버 연결 요청
    connect(
        clientSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)
    );

    while (true)
    {
        std::string message;

        std::cout << "Message: ";
        std::getline(std::cin, message);

        // 서버로 메시지 전송
        send(
            clientSocket,
            message.c_str(),
            message.size(),
            0
        );

        // 서버 응답 수신
        char buffer[1024] = {};

        int receivedBytes = recv(
            clientSocket,
            buffer,
            sizeof(buffer) - 1,
            0
        );

        if (receivedBytes <= 0)
        {
            std::cout << "Server disconnected" << std::endl;
            break;
        }

        std::cout << "Server: " << buffer << std::endl;
    }

    //사용한 소켓 종료
    close(clientSocket);

    return 0;
}