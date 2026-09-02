#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>

void receiveMessages(int clientSocket)
{
    while (true)
    {
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

        std::cout << "\nClient: " << buffer << std::endl;
    }
}

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

    std::thread receiveThread(receiveMessages, clientSocket);

    while (true)
    {
        std::string message;

        std::cout << "Message: ";
        std::getline(std::cin, message);

        send(
            clientSocket,
            message.c_str(),
            message.size(),
            0
        );
    }

    //사용한 소켓 종료
    close(clientSocket);

    return 0;
}