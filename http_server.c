#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <dirent.h>

#define HOST_PORT 80
#define MAX_CONNECTION_QUEUE 128
#define MAX_EPOLL_EVENTS 64
#define MAX_CLIENTS 2048
#define MAX_RESPONSES 2048 // Can (should) be made dynamic
#define MAX_RESPONSE_PATH_LENGTH 128

#define RECIEVE_BUFFER_SIZE 1000

void send_404_response(int client_fd);
int load_responses();

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
    responses_end = responses;
    if (!load_responses()) {
        return 1;
    }

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

int load_responses() {
    char file_path[MAX_RESPONSE_PATH_LENGTH] = "./responses/";
    char *file_name_start = file_path + 12;
    DIR *response_directory = opendir(file_path);
    if (response_directory == NULL) {
        perror("Error opening directory \"./responses\"");
        return 0;
    }
    struct response *responses_array_end = responses + MAX_RESPONSES;

    struct dirent* entry;
    while (1) {
        entry = readdir(response_directory);
        if (entry == NULL) {
            break;
        }
        if (entry->d_type == DT_DIR) {
            continue;
        }
        if (responses_end == responses_array_end) {
            fprintf(stderr, "Reached limit of %d responses", MAX_RESPONSES);
            break;
        }
        char *path_pos = file_name_start;
        char *file_name_pos = entry->d_name;
        while (*file_name_pos != 0) {
            *path_pos = *file_name_pos;
            ++path_pos;
            ++file_name_pos;
            if (path_pos > file_path + MAX_RESPONSE_PATH_LENGTH) {
                fprintf(stderr, "File name too long %s\n", entry->d_name);
                continue;
            }
        }
        *path_pos = 0;
        printf("%s\n", file_path);
        FILE *entry_file = fopen(file_path, "rb");
        if (entry_file == NULL) {
            perror("Failed to open response file");
            continue;
        }
        fseek(entry_file, 0, SEEK_END);
        long file_size = ftell(entry_file);
        if (file_size < 0) {
            perror("Failed to determine file size");
            continue;
        }
        rewind(entry_file);
        int digit_order = 10;
        int digits = 1;
        while (file_size >= digit_order) {
            ++digits;
            digit_order *= 10;
        }
        int response_length = 32 + digits + 4 + file_size; 
        responses_end->response = malloc(response_length);
        char *response_pos = responses_end->response;
        if (response_pos == NULL) {
            fprintf(stderr, "Failed to allocate memory for response file: %s\n", entry->d_name);
            continue;
        };
        char *start_response_pos = "HTTP/1.1 200 OK\r\nContent-Length:";
        char *start_response_end = start_response_pos + 32;
        while (start_response_pos < start_response_end) {
            *response_pos = *start_response_pos;
            ++response_pos;
            ++start_response_pos; 
        }
        int size_copy = file_size;
        while (digit_order > 9) {
            digit_order /= 10;
            int digit_value = size_copy / digit_order;
            size_copy -= digit_value * digit_order;
            *response_pos = '0' + digit_value;
            printf("%c\n", *response_pos);
            ++response_pos;
        }
        *response_pos = '\r';
        *(++response_pos) = '\n';
        *(++response_pos) = '\r';
        *(++response_pos) = '\n';
        ++response_pos;
        if (fread(response_pos, 1, file_size, entry_file) != (size_t)file_size) {
            fprintf(stderr, "Failed to read whole response file: %s\n", entry->d_name);
            continue;
        }
        fclose(entry_file);
        responses_end->response_length = response_length;
        responses_end->url = entry->d_name;
        ++responses_end;
    }
    return 1;
}

void send_404_response(int client_fd) {
    char *response = "HTTP/1.1 404 Not Found\r\nContent-Length:0\r\n\r\n";
    send(client_fd, response, 44, 0);
}