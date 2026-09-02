#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

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

    //사용한 소켓 종료
    close(clientSocket);

    return 0;
}