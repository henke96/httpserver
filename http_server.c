#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
#define MAX_RESPONSES 2048 // Can (should) be made dynamic

#define RECIEVE_BUFFER_SIZE 1000

void send_404_response(int client_fd);
void load_responses();

struct response {
    char *url;
    char *response;
    int response_length;
};

struct Client_socket_info {
    int fd;
    char receive_buffer[RECIEVE_BUFFER_SIZE + 1]; // + 1 so we can add null terminator for printing
    char *receive_end;
    char *current_line_start;
    int read_state; // 0: Start, 1: headers, 2: body

    int last_method; // 0: get, 1: other
    void *last_url_resource; // context given by last_method 
};

struct epoll_event epoll_events[MAX_EPOLL_EVENTS];

struct Client_socket_info client_socket_infos[MAX_CLIENTS] = {[0 ... MAX_CLIENTS - 1] = {.fd = -1}};
int client_socket_infos_position = 0;

struct response responses[MAX_RESPONSES];
struct response *responses_end;

int main(int argc, char *argv[]) {
    load_responses();

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
                            char *receive_buffer = client_socket_info->receive_buffer;
                            client_socket_info->receive_end = receive_buffer;
                            client_socket_info->current_line_start = receive_buffer;
                            client_socket_info->read_state = 0;
                            client_socket_info->last_url_resource = 0;
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
                char *receive_buffer = client_socket_info->receive_buffer;
                printf("event %d\n", fd);
                while (1) {
                    size_t max_read_length = RECIEVE_BUFFER_SIZE + receive_buffer - client_socket_info->receive_end;
                    if (max_read_length <= 0) {
                        printf("Closing client since buffer ran out\n");
                        close(fd);
                        break;
                    }
                    ssize_t read_length = recv(fd, client_socket_info->receive_end, max_read_length, 0);
    
                    if (read_length < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("Error recieving message from client, closing it");
                            client_socket_info->fd = -1;
                            close(fd);
                        }
                        break;
                    } else if (read_length == 0) {
                        printf("Recieved 0 bytes, closing client with fd %d\n", fd);
                        client_socket_info->fd = -1;
                        close(fd);
                        break;
                    } else {
                        client_socket_info->receive_end += read_length;
                        
                        char *read_pos = client_socket_info->current_line_start;
                        while (read_pos < client_socket_info->receive_end) {
                            if (*read_pos == '\r' && *(read_pos + 1) == '\n') {
                                if (read_pos == client_socket_info->current_line_start) { // Line only contained '\r\n'
                                    *(read_pos + 2) = 0;
                                    printf("%s\n", receive_buffer);
                                    if (client_socket_info->last_url_resource == 0) {
                                        send_404_response(fd);
                                    } else {
                                        struct response *response = (struct response *)client_socket_info->last_url_resource;
                                        send(fd, response->response, response->response_length, 0);
                                    }
                                    client_socket_info->receive_end = receive_buffer;
                                    client_socket_info->current_line_start = receive_buffer;
                                    client_socket_info->read_state = 0;
                                    client_socket_info->last_url_resource = 0;
                                    break; 
                                }
                                if (client_socket_info->read_state == 0) {
                                    char *parse_pos = client_socket_info->current_line_start;
                                    if (*parse_pos == 'G' && *(++parse_pos) == 'E' && *(++parse_pos) == 'T') {
                                        client_socket_info->last_method = 0; // Last method is GET
                                        parse_pos += 3; // Skip space and slash
                                        char *url_start_pos = parse_pos;
                                        for (struct response *response_pos = responses; response_pos < responses_end; ++response_pos) {
                                            char *response_url_pos = response_pos->url;
                                            while (1) {
                                                char parse_char = *parse_pos;
                                                char response_url_char = *response_url_pos;
                                                if (parse_char == response_url_char) {
                                                    ++parse_pos;
                                                    ++response_url_pos;
                                                    continue;
                                                }
                                                if (parse_char == ' ' && response_url_char == 0) {
                                                    client_socket_info->last_url_resource = response_pos;
                                                    client_socket_info->read_state = 1;
                                                    goto done_searching_response;
                                                }
                                                parse_pos = url_start_pos;
                                                break;
                                            }
                                        }                                                                                
                                    }
                                }
                                done_searching_response:
                                read_pos += 2;
                                client_socket_info->current_line_start = read_pos;
                            } else {
                                ++read_pos;
                            }
                        }
                    }
                }         
            }
        }
    }
    return 0;
}

void load_responses() {
    responses->url = "test";
    responses->response = "HTTP/1.1 200 OK\r\nContent-Length:38\r\nContent-Type: text/html\r\n\r\n<html><body>Hello World!</body></html>";
    responses->response_length = strlen(responses->response);
    responses_end = responses + 1;
}

void send_404_response(int client_fd) {
    char *response = "HTTP/1.1 404 Not Found\r\nContent-Length:0\r\n\r\n";
    send(client_fd, response, 44, 0);
}