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

    std::cout << "Connected to server!" << std::endl;

    //서버로 메세지 전송
    const char* message = "Helloooooooo Server!";

    send(
        clientSocket,
        message,
        strlen(message),
        0
    );

    // 서버가 보낸 응답 수신
    char buffer[1024] = {};

    int receivedBytes = recv(
        clientSocket,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if (receivedBytes > 0)
    {
        std::cout << "" << buffer << std::endl;
    }

    //사용한 소켓 종료
    close(clientSocket);

    return 0;
}