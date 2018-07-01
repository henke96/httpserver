#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>

#define HOST_PORT 80
#define MAX_CONNECTION_QUEUE 128
#define MAX_EPOLL_EVENTS 64
#define MAX_CLIENTS 2048

#define RECIEVE_BUFFER_SIZE 1000

void process_request(int client_fd);
void send_test_response(int client_fd);

struct http_request_progress {
    int buffer_position;
    int state;
};

struct Client_socket_info {
    int fd;
    char recieve_buffer[RECIEVE_BUFFER_SIZE + 1]; // + 1 so we can add null terminator for printing
    int recieve_length;
    int current_line_start_position;
};

struct epoll_event epoll_events[MAX_EPOLL_EVENTS];

struct Client_socket_info client_socket_infos[MAX_CLIENTS] = {[0 ... MAX_CLIENTS - 1] = {.fd = -1}};
int client_socket_infos_position = 0;

int main(int argc, char *argv[]) {
    int listen_socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if (listen_socket_fd < 0) {
        perror("Error creating socket");
        return 1;
    }

    int enable = 1;
    if (setsockopt(listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        perror("Error setting SO_REUSEADDR option");
        return 1;
    }

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(HOST_PORT);

    if (bind(listen_socket_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("Error binding socket");
        return 1;
    }

    if (listen(listen_socket_fd, MAX_CONNECTION_QUEUE) < 0) {
        perror("Error listening to socket");
        return 1;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("Error creating epoll instance");
        return 1;
    }

    struct epoll_event listen_socket_event;
    listen_socket_event.events = EPOLLIN | EPOLLET;
    listen_socket_event.data.ptr = &listen_socket_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket_fd, &listen_socket_event) < 0) {
        perror("Error registering listen socket fd to epoll");
        return 1;
    }

    socklen_t sockaddr_in_size = sizeof(struct sockaddr_in);
    while (1) {
        int num_events = epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_EVENTS, -1);
    
        for (int i = 0; i < num_events; ++i) {
            if (*((int *)epoll_events[i].data.ptr) == listen_socket_fd) {
                printf("listen socket\n");

                while (1) {
                    int new_socket_fd = accept4(listen_socket_fd, (struct sockaddr *)&listen_addr, &sockaddr_in_size, SOCK_NONBLOCK);
                    if (new_socket_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("Error accepting client socket");
                        continue;
                    }
                    printf("Accepted %d\n", new_socket_fd);

                    int start_position = client_socket_infos_position;
                    while (1) {
                        struct Client_socket_info *client_socket_info = client_socket_infos + client_socket_infos_position;
                        if (++client_socket_infos_position == MAX_CLIENTS) {
                            client_socket_infos_position = 0;
                        }
                        if (client_socket_info->fd == -1) {
                            struct epoll_event new_socket_event;
                            new_socket_event.events = EPOLLIN | EPOLLET;
                            new_socket_event.data.ptr = client_socket_info;
                            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket_fd, &new_socket_event) < 0) {
                                perror("Error registering new client socket to epoll");
                                close(new_socket_fd);
                                break;
                            }
                            client_socket_info->fd = new_socket_fd;
                            client_socket_info->recieve_length = 0;
                            client_socket_info->current_line_start_position = 0;
                            break;
                        }
                        if (client_socket_infos_position == start_position) {
                            printf("Rejected connection due to limit of %d clients reached\n", MAX_CLIENTS);
                            close(new_socket_fd);
                            break;
                        }   
                    }           
                }
            } else {
                struct Client_socket_info *client_socket_info = epoll_events[i].data.ptr;
                int fd = client_socket_info->fd;
                char *recieve_buffer = client_socket_info->recieve_buffer;
                printf("event %d\n", fd);
                while (1) {
                    size_t read_len = RECIEVE_BUFFER_SIZE - client_socket_info->recieve_length;
                    if (read_len < 1) {
                        printf("Closing client since buffer ran out\n");
                        close(fd);
                        break;
                    }
                    ssize_t read_size = recv(fd, recieve_buffer + client_socket_info->recieve_length, read_len, 0);
    
                    if (read_size < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("Error recieving message from client, closing it");
                            client_socket_info->fd = -1;
                            close(fd);
                        }
                        break;
                    } else if (read_size == 0) {
                        printf("Recieved 0 bytes, closing client with fd %d\n", fd);
                        client_socket_info->fd = -1;
                        close(fd);
                        break;
                    } else {
                        client_socket_info->recieve_length += read_size;
                
                        for (int i = client_socket_info->current_line_start_position; i + 1 < client_socket_info->recieve_length;) {
                            if (recieve_buffer[i] == '\r' && recieve_buffer[i + 1] == '\n') {
                                if (client_socket_info->current_line_start_position == i) {
                                    recieve_buffer[i + 2] = 0;
                                    printf("%s\n", recieve_buffer);
                                    send_test_response(fd);
                                    client_socket_info->recieve_length = 0;
                                    client_socket_info->current_line_start_position = 0;
                                    break; 
                                }
                                if (i + 2 < client_socket_info->recieve_length) {
                                    i += 2;
                                    client_socket_info->current_line_start_position = i;
                                    continue;
                                }
                                break;
                            }
                            ++i;
                        }
                    }
                }         
            }
        }
    }
    return 0;
}

void send_test_response(int client_fd) {
    char *response = "HTTP/1.1 200 OK\r\nContent-Length: 38\r\nContent-Type: text/html\r\n\r\n<html><body>Hello World!</body></html>";
    send(client_fd, response, strlen(response), 0);
}