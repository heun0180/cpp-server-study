#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include "protocol.h"

struct ClientInfo
{
    int socket;
    std::string nickname;
    std::string roomId;
};

void receiveMessages(
    int clientSocket,
    std::vector<ClientInfo>& clients,
    std::mutex& clientsMutex
)
{
    while (true)
    {
        std::string receivedMessage;

        if (!protocol::receiveMessage(clientSocket, receivedMessage, protocol::maxMessageSize))
        {
            break;
        }

        std::string nickname;
        std::string roomId;

        // 메시지를 보낸 클라이언트의 닉네임과 방 번호 찾기
        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            for (const ClientInfo& client : clients)
            {
                if (client.socket == clientSocket)
                {
                    nickname = client.nickname;
                    roomId = client.roomId;
                    break;
                }
            }
        }

        // 닉네임 + 메시지 생성
        std::string message =
            "[" + nickname + "] " +
            receivedMessage;

        std::cout << message << std::endl;

        // 같은 방 클라이언트에게만 메시지 전송
        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            for (const ClientInfo& client : clients)
            {
                // 자기 자신에게는 보내지 않음
                if (client.socket == clientSocket)
                    continue;

                // 다른 방이면 보내지 않음
                if (client.roomId != roomId)
                    continue;

                if (!protocol::sendMessage(client.socket, message))
                    shutdown(client.socket, SHUT_RDWR);
            }
        }
    }

    std::string nickname;
    std::string roomId;

    {
        std::lock_guard<std::mutex> lock(clientsMutex);

        // 삭제하기 전에 퇴장한 클라이언트 정보 찾기
        for (const ClientInfo& client : clients)
        {
            if (client.socket == clientSocket)
            {
                nickname = client.nickname;
                roomId = client.roomId;
                break;
            }
        }

        std::cout
            << nickname
            << " Client disconnected"
            << std::endl;

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

        // 같은 방에 남아있는 클라이언트에게만 퇴장 알림
        for (const ClientInfo& client : clients)
        {
            if (client.roomId != roomId)
                continue;

            if (!protocol::sendMessage(client.socket, closeSocketMessage))
                shutdown(client.socket, SHUT_RDWR);
        }
    }

    close(clientSocket);
}

int main()
{
    // TCP 통신에 사용할 소켓 생성
    int serverSocket = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (serverSocket == -1)
    {
        std::cout
            << "Socket creation failed"
            << std::endl;

        return 1;
    }

    std::cout
        << "Socket created: "
        << serverSocket
        << std::endl;

    // 서버의 주소와 포트 설정
    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(7777);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    // 소켓을 7777 포트에 연결
    int result = bind(
        serverSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)
    );

    if (result == -1)
    {
        std::cout
            << "Bind failed"
            << std::endl;

        close(serverSocket);

        return 1;
    }

    std::cout
        << "Bind success: port 7777"
        << std::endl;

    // 클라이언트 접속 대기 준비
    int listenResult = listen(
        serverSocket,
        5
    );

    std::vector<ClientInfo> clients;
    std::mutex clientsMutex;

    if (listenResult == -1)
    {
        std::cout
            << "Listen failed: "
            << std::strerror(errno)
            << std::endl;

        close(serverSocket);

        return 1;
    }

    std::cout
        << "Server is listening on port 7777"
        << std::endl;

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
            std::cout
                << "Accept failed"
                << std::endl;

            continue;
        }

        // 닉네임 받기
        std::string nickname;

        if (!protocol::receiveMessage(clientSocket, nickname, protocol::maxNicknameSize))
        {
            close(clientSocket);
            continue;
        }

        // 중복 닉네임 검사
        bool nicknameExists = false;

        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            for (const ClientInfo& client : clients)
            {
                if (client.nickname == nickname)
                {
                    nicknameExists = true;
                    break;
                }
            }
        }

        if (nicknameExists)
        {
            std::string message =
                "이미 사용 중인 닉네임입니다.";

            protocol::sendMessage(clientSocket, message);

            close(clientSocket);

            continue;
        }

        // 방 번호 받기
        std::string roomId;

        if (!protocol::receiveMessage(clientSocket, roomId, protocol::maxRoomIdSize))
        {
            close(clientSocket);
            continue;
        }

        std::cout
            << nickname
            << " joined room "
            << roomId
            << std::endl;

        // 접속한 클라이언트 저장
        {
            std::lock_guard<std::mutex> lock(clientsMutex);

            clients.push_back({
                clientSocket,
                nickname,
                roomId
            });

            std::cout
                << nickname
                << " connected!"
                << std::endl;

            std::cout
                << "Connected clients: "
                << clients.size()
                << std::endl;

            // 접속 메시지 생성
            std::string message =
                "[" + nickname + "]님이 접속했습니다.";

            // 같은 방에 있는 클라이언트에게만 접속 알림
            for (const ClientInfo& client : clients)
            {
                if (client.socket == clientSocket)
                    continue;

                if (client.roomId != roomId)
                    continue;

                if (!protocol::sendMessage(client.socket, message))
                    shutdown(client.socket, SHUT_RDWR);
            }
        }

        std::thread(
            receiveMessages,
            clientSocket,
            std::ref(clients),
            std::ref(clientsMutex)
        ).detach();
    }

    close(serverSocket);

    return 0;
}
