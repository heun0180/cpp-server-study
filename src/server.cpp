#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

int main(){
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(serverSocket == -1)
    {
        std::cout << "Socket creation failed" << std::endl;
        return 1;
    }

    std::cout << "Socket created: " << serverSocket << std::endl;

    close(serverSocket);

    return 0;
}