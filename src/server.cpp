#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

int main()
{

    // tcp 통신에 사용할 소켓 생성
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(serverSocket == -1)
    {
        std::cout << "Socket creation failed" << std::endl;
        return 1;
    }

    std::cout << "Socket created:" << serverSocket << std::endl;
    

    //서버의 주소와 포트 설정
    sockaddr_in ServerAddress{};

    ServerAddress.sin_family = AF_INET;
    ServerAddress.sin_port = htons(7777);
    ServerAddress.sin_addr.s_addr = INADDR_ANY;


    // 소켓을 7777 포트에 연결
    int result = bind(
        serverSocket,
        reinterpret_cast<sockaddr*>(&ServerAddress),
        sizeof(ServerAddress)
    );

    if (result == -1)
    {
        std::cout << "Bind failed" << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "Bind success:port 7777 " << std::endl;

    //클라이언트 접속 대기 준비
    int listenresult = listen(serverSocket, 5);

    if (listenresult == -1)
    {
        std::cout << "Listen failed: " << std::strerror(errno) << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "Server is listening on port 7777 " << std::endl;

    //클라이언트 접속 대기
    int clientSocket = accept(serverSocket, nullptr, nullptr);

    if (clientSocket == -1)
    {
        std::cout << "Accept failed: " << std::strerror(errno) << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "Client connected!" << std::endl;

    //클라이언트가 보낸 데이터 수신
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

        // 클라이언트에게 응답 전송
        const char* response = "Hello Client!";

        send(
            clientSocket,
            response,
            strlen(response),
            0
        );
    }

    // 소켓 종료
    close(clientSocket);
    close(serverSocket);

    return 0;
}