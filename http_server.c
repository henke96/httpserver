#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#define HOST_PORT 80
#define RECIEVE_BUFFER_SIZE 1000

void process_request(int client_fd);
void send_test_response(int client_fd);

struct http_request_progress {
    int buffer_position;
    int state;
};

int main(int argc, char *argv[]) {
    int listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_socket_fd < 0) {
        printf("Error creating socket: %s\n", strerror(errno));
        return 1;
    }

    int enable = 1;
    if (setsockopt(listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        printf("Error setting SO_REUSEADDR option: %s\n", strerror(errno));
    }

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(HOST_PORT);

    if (bind(listen_socket_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        printf("Error binding socket: %s\n", strerror(errno));
        return 1;
    }

    if (listen(listen_socket_fd, 3) < 0) {
        printf("Error listening to socket: %s\n", strerror(errno));
        return 1;
    }

    socklen_t sockaddr_in_size = sizeof(struct sockaddr_in);
    while (1) {
        int client_socket_fd = accept(listen_socket_fd, (struct sockaddr *)&listen_addr, &sockaddr_in_size);
        if (client_socket_fd < 0) {
            printf("Error accepting client socket: %s\n", strerror(errno));
            continue;
        }

        process_request(client_socket_fd);
        close(client_socket_fd);
    }
    return 0;
}

//Process HTTP-request
void process_request(int client_fd) {
    struct http_request_progress progress;
    progress.buffer_position = 0;
    progress.state = 0;
    
    int recieve_length = 0;
    while (1) {
        char recieve_buffer[RECIEVE_BUFFER_SIZE + 1];
        ssize_t read_size = recv(client_fd, recieve_buffer + recieve_length, RECIEVE_BUFFER_SIZE - recieve_length, 0);
        printf("%d\n", read_size);
        if (read_size < 0) {
            printf("Error recieving message: %s\n", strerror(errno));
            break;
        } else if (read_size == 0) {
            printf("Connection closed or buffer full\n");
            break;
        } else {
            recieve_length += read_size;
            
            for (int i = progress.buffer_position; i + 1 < recieve_length;) {
                if (recieve_buffer[i] == '\r' && recieve_buffer[i + 1] == '\n') {
                    if (i == progress.buffer_position) {
                        recieve_buffer[i + 2] = 0;
                        printf("%s\n", recieve_buffer);
                        send_test_response(client_fd);
                        return; 
                    }
                    if (i + 2 < recieve_length) {
                        i += 2;
                        progress.buffer_position = i;
                        continue;
                    }
                    break;
                }
                ++i;
            }
        }
    }
}

void send_test_response(int client_fd) {
    char *response = "HTTP/1.1 200 OK\r\nContent-Length: 38\r\nContent-Type: text/html\r\n\r\n<html><body>Hello World!</body></html>";
    send(client_fd, response, strlen(response), 0);
}