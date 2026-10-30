#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include <system_error>
#include "protocol.h"

struct ClientInfo
{
    int socket;
    std::string nickname;
    std::string roomId;
    bool closing = false;
};

// clientsMutex를 잡은 상태에서 호출한다. close는 해당 수신 스레드가 담당한다.
void sendToClient(ClientInfo& client, const std::string& message)
{
    if (client.closing)
        return;

    protocol::IoResult result = protocol::sendMessage(client.socket, message);
    if (!result)
    {
        client.closing = true;
        std::cout << "Send failed for " << client.nickname << ": "
            << protocol::resultMessage(result) << std::endl;
        shutdown(client.socket, SHUT_RDWR);
    }
}

bool registerClient(
    int clientSocket,
    std::vector<ClientInfo>& clients,
    std::timed_mutex& clientsMutex,
    std::chrono::steady_clock::time_point deadline
)
{
    std::string nickname;
    protocol::IoResult result = protocol::receiveMessage(
        clientSocket, nickname, protocol::maxNicknameSize, deadline
    );
    if (!result)
    {
        std::cout << "Nickname receive ended: " << protocol::resultMessage(result) << std::endl;
        return false;
    }

    std::string roomId;
    result = protocol::receiveMessage(clientSocket, roomId, protocol::maxRoomIdSize, deadline);
    if (!result)
    {
        std::cout << "Room ID receive ended: " << protocol::resultMessage(result) << std::endl;
        return false;
    }

    std::unique_lock<std::timed_mutex> lock(clientsMutex, std::defer_lock);
    if (!lock.try_lock_until(deadline) || std::chrono::steady_clock::now() >= deadline)
    {
        std::cout << "Registration deadline exceeded (socket " << clientSocket << ")" << std::endl;
        return false;
    }

    // 중복 검사와 목록 추가를 같은 잠금 안에서 처리한다.
    for (const ClientInfo& client : clients)
    {
        if (client.nickname != nickname)
            continue;

        lock.unlock();
        result = protocol::sendMessage(clientSocket, "이미 사용 중인 닉네임입니다.");
        if (!result)
            std::cout << "Nickname reply failed: " << protocol::resultMessage(result) << std::endl;
        return false;
    }

    clients.push_back({clientSocket, nickname, roomId});

    std::cout << nickname << " joined room " << roomId << std::endl;
    std::cout << nickname << " connected!" << std::endl;
    std::cout << "Connected clients: " << clients.size() << std::endl;

    std::string message = "[" + nickname + "]님이 접속했습니다.";
    for (ClientInfo& client : clients)
    {
        if (client.socket == clientSocket || client.roomId != roomId)
            continue;

        sendToClient(client, message);
    }

    return true;
}

void receiveMessages(
    int clientSocket,
    std::vector<ClientInfo>& clients,
    std::timed_mutex& clientsMutex,
    std::chrono::steady_clock::time_point registrationDeadline
)
{
    if (!registerClient(clientSocket, clients, clientsMutex, registrationDeadline))
    {
        close(clientSocket);
        return;
    }

    while (true)
    {
        std::string receivedMessage;

        protocol::IoResult result = protocol::receiveMessage(
            clientSocket,
            receivedMessage,
            protocol::maxMessageSize
        );

        if (!result)
        {
            if (result.status != protocol::IoStatus::Closed)
            {
                std::cout << "Receive failed (socket " << clientSocket << "): "
                    << protocol::resultMessage(result) << std::endl;
            }
            break;
        }

        std::string nickname;
        std::string roomId;
        bool closing = false;

        // 메시지를 보낸 클라이언트의 닉네임과 방 번호 찾기
        {
            std::lock_guard<std::timed_mutex> lock(clientsMutex);

            for (const ClientInfo& client : clients)
            {
                if (client.socket == clientSocket)
                {
                    nickname = client.nickname;
                    roomId = client.roomId;
                    closing = client.closing;
                    break;
                }
            }
        }

        if (closing)
            break;

        // 닉네임 + 메시지 생성
        std::string message =
            "[" + nickname + "] " +
            receivedMessage;

        std::cout << message << std::endl;

        // 같은 방 클라이언트에게만 메시지 전송
        {
            std::lock_guard<std::timed_mutex> lock(clientsMutex);

            for (ClientInfo& client : clients)
            {
                // 자기 자신에게는 보내지 않음
                if (client.socket == clientSocket)
                    continue;

                // 다른 방이면 보내지 않음
                if (client.roomId != roomId)
                    continue;

                sendToClient(client, message);
            }
        }
    }

    std::string nickname;
    std::string roomId;

    {
        std::lock_guard<std::timed_mutex> lock(clientsMutex);

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
        for (ClientInfo& client : clients)
        {
            if (client.roomId != roomId)
                continue;

            sendToClient(client, closeSocketMessage);
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
    std::timed_mutex clientsMutex;

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

        // 닉네임과 방 번호 수신에 같은 마감 시각을 사용한다.
        auto registrationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        protocol::IoResult result = protocol::configureSocket(clientSocket);
        if (!result)
        {
            std::cout << "Socket setup failed: " << protocol::resultMessage(result) << std::endl;
            close(clientSocket);
            continue;
        }

        try
        {
            std::thread(
                receiveMessages,
                clientSocket,
                std::ref(clients),
                std::ref(clientsMutex),
                registrationDeadline
            ).detach();
        }
        catch (const std::system_error& error)
        {
            std::cout << "Client thread creation failed: " << error.what() << std::endl;
            close(clientSocket);
        }
    }

    close(serverSocket);

    return 0;
}
