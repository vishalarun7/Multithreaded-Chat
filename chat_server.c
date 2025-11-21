
#include <stdio.h>
#include <stdlib.h>
#include "udp.h"
#include <ctype.h>
int main(int argc, char *argv[])
{
    int sd = udp_socket_open(SERVER_PORT);
    assert(sd > -1);
    while (1) 
    {
        char client_request[BUFFER_SIZE], server_response[BUFFER_SIZE];

        printf("Server is listening on port %d\n", SERVER_PORT);

        struct sockaddr_in client_address;
    

        int rc = udp_socket_read(sd, &client_address, client_request, BUFFER_SIZE);

        if (rc > 0)
        {
            int has_upper = 0; 
            int has_lower = 0; 

            strcpy(server_response, "received: ");
            strcat(server_response, client_request);
            char client_ip[INET_ADDRSTRLEN];
            strcpy(client_ip, inet_ntoa(client_address.sin_addr));

            strcat(server_response, " (From IP");
            strcat(server_response, client_ip);
            strcat(server_response, ")\n");

            for (int i = 0; client_request[i] != '\0'; i++){
                if (isupper((unsigned char)client_request[i])) {
                    has_upper = 1;
                }
                if (islower((unsigned char)client_request[i])) {
                    has_lower = 1;
                }
            }

            if (has_upper && has_lower) {
                strcat(server_response, "[Mixed Case]");
            } else if (has_upper) {
                strcat(server_response, "[ALL UPPERCASE]");
            } else if (has_lower) {
                strcat(server_response, " all lowercase]");
            } else {
                strcat(server_response, "[No letters]");
            }
            rc = udp_socket_write(sd, &client_address, server_response, BUFFER_SIZE);
        }
    }

    return 0;
}