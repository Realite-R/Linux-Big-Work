#include "protocol.h"
#include "logger.h"
#include "server_log.h"
#include "session.h"
#include "session_store.h"
#include "ai_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
    int client_fd;
    char *response;
    size_t response_length;
    size_t response_capacity;
} stream_context_t;

static int append_response(stream_context_t *context, const char *text, size_t length) {
    if (length > SIZE_MAX - context->response_length - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t needed = context->response_length + length + 1;
    if (needed > context->response_capacity) {
        size_t capacity = context->response_capacity == 0 ? 1024 : context->response_capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        char *resized = realloc(context->response, capacity);
        if (resized == NULL) return -1;
        context->response = resized;
        context->response_capacity = capacity;
    }
    memcpy(context->response + context->response_length, text, length);
    context->response_length += length;
    context->response[context->response_length] = '\0';
    return 0;
}

static int forward_model_chunk(const char *text, size_t length, void *userdata) {
    stream_context_t *context = userdata;
    char *terminated = malloc(length + 1);
    if (terminated == NULL) return -1;
    // 把要发送的内容存在terminate里
    memcpy(terminated, text, length);
    terminated[length] = '\0';

    // 向客户端发送内容
    int result = protocol_send_text(context->client_fd, "DATA", terminated);
    free(terminated);
    if (result < 0) return -1;
    return append_response(context, text, length);
}

static void reap_children(int signal_number) {
    (void)signal_number;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Reap every child that has already exited. */
    }
    errno = saved_errno;
}

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
这个函数是处理一个客户端的多次请求的函数
*/
static int send_literal(int fd, const char *text) {
    return protocol_send_all(fd, text, strlen(text));
}

static int send_list_entry(unsigned long id, time_t updated, int busy,
                           const char *title, void *userdata) {
    int fd = *(int *)userdata;
    char kind[128];
    snprintf(kind, sizeof(kind), "SESSION %lu %lld %d", id, (long long)updated, busy);
    return protocol_send_frame(fd, kind, title);
}

static int send_history(int fd, const stored_session_t *stored) {
    char kind[128];
    snprintf(kind, sizeof(kind), "OPENED %lu", stored->id);
    if (protocol_send_frame(fd, kind, stored->title) < 0) return -1;
    for (size_t i = 0; i < stored->count; ++i) {
        const stored_message_t *message = &stored->messages[i];
        char begin[64];
        snprintf(begin, sizeof(begin), "MESSAGE %c %d\n", message->role, message->context);
        if (send_literal(fd, begin) < 0 ||
            protocol_send_text(fd, "TEXT", message->text) < 0 ||
            send_literal(fd, "MESSAGE_END\n") < 0) return -1;
    }
    return send_literal(fd, "HISTORY_END\n");
}

static int open_log(logger_t *logger, const char *directory, unsigned long id) {
    char path[LOGGER_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/logs", directory) >= (int)sizeof(path)) return -1;
    return logger_open(logger, path, id);
}

static void handle_client(int client_fd, const char *directory) {
    // 子进程负责当前客户端的多次操作；client_fd 是连接描述符，directory 是会话存储目录
    // line 保存原始协议帧，question 保存去掉 Q 前缀并还原转义后的问题正文
    char line[PROTOCOL_MAX_LINE], question[PROTOCOL_MAX_INPUT];
    // stored 持有完整会话历史和文件锁；-1 表示尚未打开锁文件
    stored_session_t stored = {.lock_fd = -1};
    // session 保存内存上下文；logger 写当前会话的聊天正文日志，不是服务活动日志
    session_t session;
    session_init(&session);
    logger_t logger = {.fd = -1};
    // 使用过会话管理命令后置为 1，要求先选会话；普通客户端则可在首次提问时自动创建
    int managed = 0;
    for (;;) {
        // 等待当前客户端的一行请求；超长行已被读取函数丢弃，可以报错后继续接收
        int result = protocol_read_line(client_fd, line, sizeof(line));
        if (result <= 0) {
            if (result == -2) { (void)protocol_send_error(client_fd, "request line is too long"); continue; }
            break;
        }
        // QUIT、连接关闭或读取失败会离开循环，统一执行末尾的资源清理
        if (strcmp(line, "QUIT") == 0) break;
        // 获取列表：开始标记 -> 多条 SESSION -> 结束标记
        // store_list 每发现一个会话，就回调 send_list_entry；&client_fd 指明发送到哪个连接
        if (strcmp(line, "SESSIONS") == 0) {
            managed = 1;
            if (send_literal(client_fd, "SESSIONS_BEGIN\n") < 0) break;
            if (store_list(directory, send_list_entry, &client_fd) < 0) {
                if (protocol_send_error(client_fd, "cannot list sessions") < 0) break;
            } else if (send_literal(client_fd, "SESSIONS_END\n") < 0) break;
            continue;
        }
        // 返回列表：关闭聊天日志、释放会话锁并清空上下文，磁盘历史和 TCP 连接仍保留
        if (strcmp(line, "LEAVE") == 0) {
            managed = 1;
            logger_close(&logger); store_close(&stored);
            session_destroy(&session); session_init(&session);
            if (send_literal(client_fd, "LEFT\n") < 0) break;
            continue;
        }
        // NEW 创建空会话；OPEN <id> 打开已有会话，两者共用后面的切换和历史发送流程
        if (strcmp(line, "NEW") == 0 || strncmp(line, "OPEN ", 5) == 0) {
            managed = 1;
            // 先准备候选会话，加载成功前不释放当前会话，避免切换失败丢掉当前状态
            stored_session_t candidate = {.lock_fd = -1};
            int opened;
            if (strcmp(line, "NEW") == 0) opened = store_create(directory, &candidate);
            else {
                // 跳过 OPEN 和空格，解析十进制 ID，并拒绝空值、尾随字符、0 和越界值
                char *end = NULL;
                errno = 0;
                unsigned long id = strtoul(line + 5, &end, 10);
                if (errno || end == line + 5 || *end || !id || line[5] < '0' || line[5] > '9') {
                    (void)protocol_send_error(client_fd, "invalid session id"); continue;
                }
                // 存储模块先获取独占锁再读历史；被其他客户端占用时立即失败，不等待
                opened = store_open(directory, id, &candidate);
            }
            if (opened < 0) {
                const char *error = errno == EWOULDBLOCK ? "session is in use" : "cannot open session";
                if (protocol_send_error(client_fd, error) < 0) break;
                continue;
            }
            // 从候选会话的成功用户/助手轮次恢复模型上下文，数量上限由 session 模块控制
            session_t restored;
            if (store_context(&candidate, &restored) < 0) {
                store_close(&candidate);
                if (protocol_send_error(client_fd, "cannot restore context") < 0) break;
                continue;
            }
            // 候选会话及上下文都准备好后，释放旧资源，再接管新对象中的指针和锁描述符
            logger_close(&logger); store_close(&stored); session_destroy(&session);
            stored = candidate; session = restored;
            // 聊天日志打不开只在服务端提示；随后发送 OPENED、完整历史和 HISTORY_END
            if (open_log(&logger, directory, stored.id) < 0) perror("server: open session log");
            if (send_history(client_fd, &stored) < 0) break;
            continue;
        }
        // 剩余请求必须是 Q <问题>；line + 2 跳过前缀，恢复正文中的换行、回车和反斜杠
        if (strncmp(line, "Q ", 2) != 0 || protocol_unescape(line + 2, question, sizeof(question)) < 0) {
            if (protocol_send_error(client_fd, "invalid request") < 0) break;
            continue;
        }
        // 尚未选择会话时：管理模式提示先选择；普通模式兼容旧客户端，自动创建会话
        if (!stored.id) {
            if (managed) { (void)protocol_send_error(client_fd, "select a session first"); continue; }
            if (store_create(directory, &stored) < 0) {
                if (protocol_send_error(client_fd, "cannot create session") < 0) break;
                continue;
            }
            if (open_log(&logger, directory, stored.id) < 0) perror("server: open session log");
        }
        // 每轮从持久历史构建临时上下文 working，再加入本次问题
        // 添加消息可能裁剪旧轮次；请求失败时丢弃 working，避免影响已保存的成功历史
        session_t working;
        if (store_context(&stored, &working) < 0) {
            (void)protocol_send_error(client_fd, "cannot restore context"); continue;
        }
        if (session_add_user(&working, question) < 0) {
            session_destroy(&working);
            (void)protocol_send_error(client_fd, "cannot store input"); continue;
        }
        // BEGIN 通知客户端开始回复；发送失败则释放临时上下文并结束当前连接处理
        if (protocol_send_begin(client_fd) < 0) { session_destroy(&working); break; }
        // stream 保存客户端描述符和累计回复缓冲区，其余字段自动初始化为 0
        stream_context_t stream = {.client_fd = client_fd};
        // 模型服务默认位于 127.0.0.1:8080，HTTP 路径为 /v1/chat/completions
        const ai_client_config_t config = {
            .host = AI_DEFAULT_HOST, .port = AI_DEFAULT_PORT, .path = AI_DEFAULT_PATH,
        };
        char error[256] = {0};
        // 模型每返回一块文本就调用 forward_model_chunk：发送 DATA，并累计完整回答
        // &stream 是传给回调的附带数据；模型调用失败时将原因写入 error
        result = ai_client_stream(&config, &working, forward_model_chunk, &stream, error, sizeof(error));
        // 没有收到正文时使用空字符串，避免后续把空指针当作回答文本
        const char *answer = stream.response ? stream.response : "";
        if (result == 0 && session_add_assistant(&working, answer) < 0) {
            result = -1; snprintf(error, sizeof(error), "cannot store reply");
        }
        // 只有生成及内存上下文更新成功，才将问题与回答作为完整一轮保存到会话文件
        if (result == 0 && store_commit_turn(&stored, question, answer) < 0) {
            result = -1; snprintf(error, sizeof(error), "cannot save conversation; turn not saved");
        }
        if (result == 0) {
            // 成功后将 working 的资源交给 session；此分支不能再销毁 working
            session_destroy(&session); session = working;
            // 可读聊天日志失败不撤销已保存会话；持久化成功后才发送 END 完成标记
            if (logger_write_user(&logger, question) < 0 || logger_write_assistant(&logger, answer) < 0)
                perror("server: write session log");
            result = protocol_send_end(client_fd);
        } else {
            // 生成或保存失败：丢弃临时上下文，发送 ERROR，本轮不进入持久历史
            session_destroy(&working);
            result = protocol_send_error(client_fd, error[0] ? error : "model request failed; turn not saved");
        }
        // 无论成功失败都释放累计回答；此时 result 已变成 END/ERROR 的发送结果
        free(stream.response);
        // 错误提示发送成功可以继续聊天；最终响应发送失败则退出当前客户端循环
        if (result < 0) break;
    }
    // 统一清理当前会话日志、完整历史、会话锁和上下文；client_fd 由调用方关闭
    logger_close(&logger); store_close(&stored); session_destroy(&session);
}

static int prepare_log_directory(const char *program_path, char *directory,
                                  size_t capacity) {
    char resolved_path[PATH_MAX];
    if (realpath(program_path, resolved_path) == NULL) {
        if (snprintf(directory, capacity, "logs") >= (int)capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        /* The executable is in project/bin; put logs beside that directory. */
        char *bin_directory = strrchr(resolved_path, '/');
        if (bin_directory == NULL) {
            if (snprintf(directory, capacity, "logs") >= (int)capacity) {
                errno = ENAMETOOLONG;
                return -1;
            }
        } else {
            *bin_directory = '\0';
            char *project_directory = strrchr(resolved_path, '/');
            if (project_directory != NULL) {
                *project_directory = '\0';
            }
            if (snprintf(directory, capacity, "%s/logs", resolved_path) >= (int)capacity) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
    }

    if (mkdir(directory, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}
/*
这个函数创建了一个端口，这个端口接受客户端配置的端口发来的信息，同时这个服务端进程监听这个端口接受的内容
端口地址为 127:0:0:1 9090
*/
static int create_listener(unsigned short port) {
    // 创建了一个TCP socket AF_INET 代表IPv4,
    // 但是这个server_fd只是用来找到这个对应的socket进程的编号
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("server: socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("server: setsockopt");
        close(server_fd);
        return -1;
    }

    // 设置要监听的IPv4地址
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    // 设置端口sin_port，转换为十六进制
    address.sin_port = htons(port);
    // 设置ip地址sin_addr，转换为二进制形式
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "server: failed to prepare loopback address\n");
        close(server_fd);
        return -1;
    }

    // 将socket进程绑定在address的通信地址下
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("server: bind");
        close(server_fd);
        return -1;
    }

    // 让这个socket进程进入监听状态，随时准备接受客户端配置的端口发来的连接请求
    if (listen(server_fd, 16) < 0) {
        perror("server: listen");
        close(server_fd);
        return -1;
    }
    return server_fd;
}

int main(int argc, char **argv) {
    unsigned short port = PROTOCOL_DEFAULT_PORT;
    // parse_port 里面已经包含了端口的写入
    if (argc > 2 || (argc == 2 && parse_port(argv[1], &port) < 0)) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 写入日志路径函数
    char log_directory[LOGGER_PATH_MAX];
    if (prepare_log_directory(argv[0], log_directory, sizeof(log_directory)) < 0) {
        perror("server: create log directory");
        return EXIT_FAILURE;
    }

    char session_directory[STORE_PATH_MAX];
    char *slash = strrchr(log_directory, '/');
    size_t prefix = slash ? (size_t)(slash - log_directory) : 0;
    int path_length = slash ? snprintf(session_directory, sizeof(session_directory), "%.*s/sessions", (int)prefix, log_directory)
                            : snprintf(session_directory, sizeof(session_directory), "sessions");
    if (path_length < 0 || path_length >= (int)sizeof(session_directory) ||
        store_init(session_directory, log_directory) < 0) {
        perror("server: initialize session store"); return EXIT_FAILURE;
    }

    // child_action是在定义信号产生之后的行为
    struct sigaction child_action;
    memset(&child_action, 0, sizeof(child_action));
    // sa_hander 是在接受到信号之后执行这个函数，核心的配置代码
    child_action.sa_handler = reap_children;
    // 不屏蔽其他信号
    sigemptyset(&child_action.sa_mask);
    child_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    // SIGCHLD 会在子进程退出之后自动向父进程发出信号
    if (sigaction(SIGCHLD, &child_action, NULL) < 0) {
        perror("server: sigaction(SIGCHLD)");
        return EXIT_FAILURE;
    }

    // 这个是处理服务端向断连的客户端发送信息时，防止报错的处理
    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    // 特殊值，代表忽略信号不行动
    pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&pipe_action.sa_mask);
    if (sigaction(SIGPIPE, &pipe_action, NULL) < 0) {
        perror("server: sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    // 创建监听服务端的socket进程
    int server_fd = create_listener(port);
    if (server_fd < 0) {
        return EXIT_FAILURE;
    }
    // 打开服务活动日志：控制台 + logs/server.log 双输出
    server_log_t server_log;
    if (server_log_open(&server_log, log_directory, 1) < 0) {
        // 日志文件打不开不阻塞服务：控制台输出仍然可用
        perror("server: open server log");
        server_log.fd = -1;
        server_log.console = 1;
        snprintf(server_log.path, sizeof(server_log.path), "(unavailable)");
    }

    char startup[SERVER_LOG_PATH_MAX + 128];
    snprintf(startup, sizeof(startup),
             "START server listening on 127.0.0.1:%u (pid=%ld, log=%s)",
             (unsigned int)port, (long)getpid(), server_log.path);
    (void)server_log_write(&server_log, startup);

    for (;;) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        // 获得和127.0.0.0:9090 建立了连接的客户端端口地址
        // 如果暂时没有客户端连接，就会堵塞在accept这一个代码
        int client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_length);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("server: accept");
            break;
        }

        // 记录客户端来源：IP 和端口
        char peer[64];
        if (inet_ntop(AF_INET, &client_address.sin_addr, peer, sizeof(peer)) == NULL) {
            snprintf(peer, sizeof(peer), "unknown");
        }
        char peer_text[96];
        snprintf(peer_text, sizeof(peer_text), "%s:%u", peer, (unsigned int)ntohs(client_address.sin_port));
        char connect_line[160];
        snprintf(connect_line, sizeof(connect_line), "CONNECT peer=%s", peer_text);
        (void)server_log_write(&server_log, connect_line);

        pid_t child = fork();
        if (child < 0) {
            perror("server: fork");
            char fork_error[160];
            snprintf(fork_error, sizeof(fork_error), "ERROR fork failed for peer=%s", peer_text);
            (void)server_log_write(&server_log, fork_error);
            (void)protocol_send_error(client_fd, "server cannot create a session process");
            close(client_fd);
            continue;
        }
        // 子进程进入这个条件分支里面
        if (child == 0) {
            // 关闭不需要监听的socket
            close(server_fd);
            handle_client(client_fd, session_directory);
            close(client_fd);
            // 子进程结束前记录自己的退出，父进程无需感知
            char exit_line[160];
            snprintf(exit_line, sizeof(exit_line), "EXIT child=%ld peer=%s", (long)getpid(), peer_text);
            (void)server_log_write(&server_log, exit_line);
            _exit(EXIT_SUCCESS);
        }
        // 父进程继续等待下一个来的客户端连接，就能够处理多个不同的客户端请求了
        char fork_line[160];
        snprintf(fork_line, sizeof(fork_line), "FORK child=%ld for peer=%s", (long)child, peer_text);
        (void)server_log_write(&server_log, fork_line);
        close(client_fd);
    }

    close(server_fd);
    server_log_close(&server_log);
    return EXIT_FAILURE;
}
