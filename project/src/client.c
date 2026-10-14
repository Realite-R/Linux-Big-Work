#include "protocol.h"
#include "client_input.h"
#include "client_ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_port(const char *text, unsigned short *port_out) {
    char *end = NULL;
    long value;

    if (text == NULL || port_out == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }
    *port_out = (unsigned short)value;
    return 0;
}

/*
用来主动连接服务端监听的网络端口的函数
*/
static int connect_to_server(const char *host, unsigned short port) {
    // 创建IPv4，TCP的网络进程socket对象
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("client: socket");
        return -1;
    }

    // address指代的是一个端口
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    // IPv4家族
    address.sin_family = AF_INET;
    // 把端口写入address
    address.sin_port = htons(port);
    // 把ip地址写入address
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "client: host must be an IPv4 address: %s\n", host);
        close(socket_fd);
        return -1;
    }
    // 连接之后返回对应的socket_fd进程
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("client: connect");
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

/*
对服务端生成的内容做出处理，其中包含了流式输出的实现
*/
static int receive_reply(int socket_fd) {
    char line[PROTOCOL_MAX_LINE];
    char payload[PROTOCOL_MAX_LINE];
    int got_begin = 0;

    for (;;) {
        // 解析服务端的数据帧的程序,在此时正文里面的转义字符没有被解析出来
        int result = protocol_read_line(socket_fd, line, sizeof(line));
        if (result == 0) {
            fprintf(stderr, "client: server closed the connection\n");
            return -1;
        }
        if (result == -1) {
            perror("client: recv");
            return -1;
        }
        if (result == -2) {
            fprintf(stderr, "client: server frame is too long\n");
            return -1;
        }
        if (strcmp(line, "BEGIN") == 0) {
            got_begin = 1;
            // 输出前面的对话框
            fputs("AI: ", stdout);
            fflush(stdout);
            continue;
        }
        if (strncmp(line, "DATA ", 5) == 0) {
            // 这个时候就要先解析一下转义字符的内容
            if (!got_begin || protocol_unescape(line + 5, payload, sizeof(payload)) < 0) {
                fprintf(stderr, "client: invalid DATA frame\n");
                return -1;
            }
            // 输出正文内容
            fputs(payload, stdout);
            fflush(stdout);
            continue;
        }
        if (strcmp(line, "END") == 0) {
            if (!got_begin) {
                fprintf(stderr, "client: unexpected END frame\n");
                return -1;
            }
            // 换行
            fputc('\n', stdout);
            return 0;
        }
        if (strncmp(line, "ERROR ", 6) == 0) {
            if (protocol_unescape(line + 6, payload, sizeof(payload)) < 0) {
                fprintf(stderr, "client: invalid ERROR frame\n");
            } else {
                fprintf(stderr, "server error: %s\n", payload);
            }
            return 1;
        }

        fprintf(stderr, "client: unknown server frame: %s\n", line);
        return -1;
    }
}

int main(int argc, char **argv) {
    const char *program_name = argv[0];
    const char *host = "127.0.0.1";
    unsigned short port = PROTOCOL_DEFAULT_PORT;

    // 检测是否使用plain选项
    int plain = argc > 1 && strcmp(argv[1], "--plain") == 0;
    if (plain) { --argc; ++argv; }

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [--plain] [host] [port]\n", program_name);
        return EXIT_FAILURE;
    }
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc == 3 && parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s [--plain] [host] [port]\n", program_name);
        return EXIT_FAILURE;
    }

    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    // 忽略这个信号
    pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&pipe_action.sa_mask);
    if (sigaction(SIGPIPE, &pipe_action, NULL) < 0) {
        perror("client: sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    int socket_fd = connect_to_server(host, port);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    // 进入全屏聊天的判断，为真时全屏聊天
    if (!plain && client_ui_available()) {
        int result = client_ui_run(socket_fd);
        if (result < 0) perror("client: session interface");
        close(socket_fd);
        return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    // 这个分支之下就是普通模式聊天，不会再额外进入一个记录
    char input[PROTOCOL_MAX_INPUT];
    for (;;) {
        // 处理光标移动等复杂问题的函数 （核心代码）
        int input_result = client_read_input(input, sizeof(input));
        if (input_result <= 0) {
            if (input_result < 0) {
                perror("client: input");
            }
            (void)protocol_send_quit(socket_fd);
            break;
        }

        // 语法输入退出
        if (strcmp(input, "/quit") == 0) {
            (void)protocol_send_quit(socket_fd);
            break;
        }
        // 将请求发出
        if (protocol_send_query(socket_fd, input) < 0) {
            perror("client: send");
            break;
        }
        // 处理服务端发挥的信息 （核心代码）
        if (receive_reply(socket_fd) < 0) {
            break;
        }
    }

    close(socket_fd);
    return EXIT_SUCCESS;
}
