#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>

struct ClientInfo
{
    int socket;
    std::string nickname;
};

void receiveMessages(int clientSocket, std::vector<ClientInfo>&clients, std::mutex& clientsMutex)
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
            break;
        }

        std::string nickname;

        // 메시지를 보낸 클라이언트의 닉네임 찾기
        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            for(const ClientInfo& client : clients)
            {
                if(client.socket == clientSocket)
                {
                    nickname = client.nickname;
                    break;
                }
            }
        }

        // 닉네임 + 메시지 생성
        std::string message =
            "[" + nickname + "] " +
            std::string(buffer, receivedBytes);

        std::cout << message << std::endl;



        std::lock_guard<std::mutex> lock(clientsMutex);
        for(const ClientInfo& client : clients)
        {
            if(client.socket == clientSocket)
                continue;

            send(client.socket, message.c_str(), message.size(), 0);
        }
    }

    std::lock_guard<std::mutex> lock(clientsMutex); 

    // 삭제하기 전에 퇴장한 클라이언트의 닉네임 찾기
    std::string nickname;
    for(const ClientInfo& client : clients)
    {
        if(client.socket == clientSocket)
        {
            nickname = client.nickname;
            break;
        }
    }

    std::cout << nickname.c_str() << " Client disconnected" << std::endl;

    // 연결 종료된 클라이언트를 목록에서 제거
    clients.erase(
        std::remove_if(
            clients.begin(),
            clients.end(),
            [clientSocket](const ClientInfo& client)
            {
                return client.socket == clientSocket;
            }
        ),
        clients.end()
    );

    // 퇴장 메시지 생성
    std::string closeSocketMessage =
    nickname + "님이 퇴장했습니다.";

    for(const ClientInfo& client : clients)
    {
        send(client.socket, closeSocketMessage.c_str(), closeSocketMessage.size(), 0);
    }

    close(clientSocket);
}

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

    std::vector<ClientInfo> clients;
    std::mutex clientsMutex;

    if (listenresult == -1)
    {
        std::cout << "Listen failed: " << std::strerror(errno) << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "Server is listening on port 7777 " << std::endl;

    while (true)
    {
        // 새로운 클라이언트 접속 대기
        int clientSocket = accept(
            serverSocket,
            nullptr,
            nullptr
        );

        if (clientSocket == -1)
        {
            std::cout << "Accept failed" << std::endl;
            continue;
        }

        char nicknameBuffer[100] = {};

        int nicknameBytes = recv(
            clientSocket,
            nicknameBuffer,
            sizeof(nicknameBuffer) - 1,
            0
        );

        if (nicknameBytes <= 0)
        {
            close(clientSocket);
            continue;
        }

        std::string nickname(
            nicknameBuffer,
            nicknameBytes
        );

        //닉네임 검사하기
        bool nicknameExists = false;

        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            for(const ClientInfo& client : clients)
            {
                if(client.nickname == nickname)
                {
                    nicknameExists = true;
                    break;
                }
            }
        }

        if(nicknameExists)
        {
            std::string message = "이미 사용 중인 닉네임입니다.";

            send(
                clientSocket,
                message.c_str(),
                message.size(),
                0
            );

            close(clientSocket);
            continue;
        }


        // 접속한 클라이언트 소켓 저장
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back({clientSocket,nickname});

            std::cout << nickname << " connected!" << std::endl;
            std::cout << "Connected clients: " << clients.size() << std::endl;

            for(const ClientInfo& client : clients)
            {
                if(client.socket == clientSocket)
                    continue;
                // 닉네임 + 메시지 생성
                std::string message = "[" + nickname + "]님이 접속했습니다.";

                send(client.socket, message.c_str(), message.size(), 0);
            }
        }           

        std::thread(receiveMessages, clientSocket, std::ref(clients), std::ref(clientsMutex)).detach();
    }


    // 소켓 종료
    close(serverSocket);

    return 0;
}