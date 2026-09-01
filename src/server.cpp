#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int main()
{
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(serverSocket == -1)
    {
        std::cout << "Socket creation failed" << std::endl;
        return 1;
    }

    sockaddr_in ServerAddress{};

    ServerAddress.sin_family = AF_INET;
    ServerAddress.sin_port = htons(7777);
    ServerAddress.sin_addr.s_addr = INADDR_ANY;

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

    close(serverSocket);

    return 0;
}