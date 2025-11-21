
#include <stdio.h>
#include "udp.h"

#define CLIENT_PORT 10000

int main(int argc, char *argv[])
{
    int sd = udp_socket_open(CLIENT_PORT);
    struct sockaddr_in server_addr, responder_addr;
    int rc = set_socket_addr(&server_addr, "127.0.0.1", SERVER_PORT);

    char client_request[BUFFER_SIZE], server_response[BUFFER_SIZE];

    printf("Enter request: ");
    fgets(client_request, BUFFER_SIZE, stdin);

    rc = udp_socket_write(sd, &server_addr, client_request, BUFFER_SIZE);

    if (rc > 0)
    {

        int rc = udp_socket_read(sd, &responder_addr, server_response, BUFFER_SIZE);
        printf("server_response: %s", server_response);
    }

    return 0;
}
