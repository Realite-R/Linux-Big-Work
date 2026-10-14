#include "ai_client.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// 本模块连接 llama-server：上下文 -> JSON 请求 -> HTTP 正文 -> SSE 行 -> 回调
// HTTP 分块、SSE 行和客户端 DATA 帧是不同层次的边界，不能直接等同
#define AI_MAX_HEADERS 16384
#define AI_MAX_SSE_LINE 16384
#define AI_MAX_ERROR 256
#define AI_MAX_TOKENS 512

// 动态字符串：length 是已用字节数，capacity 是容量，data 保存文本及结束符
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_builder_t;

// 格式化错误到调用方缓冲区，不在这里直接打印或发送给客户端
static void set_error(char *buffer, size_t capacity, const char *format, ...) {
    if (buffer == NULL || capacity == 0) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
}

// 检查容量计算溢出，为新增正文和结束符预留空间，按倍数扩容
static int builder_reserve(string_builder_t *builder, size_t additional) {
    if (additional > SIZE_MAX - builder->length - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t required = builder->length + additional + 1;
    if (required <= builder->capacity) {
        return 0;
    }
    size_t capacity = builder->capacity == 0 ? 256 : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = realloc(builder->data, capacity);
    if (data == NULL) {
        return -1;
    }
    builder->data = data;
    builder->capacity = capacity;
    return 0;
}

// 按字节追加内容，更新长度并补上字符串结束符
static int builder_append(string_builder_t *builder, const char *text, size_t length) {
    if (builder_reserve(builder, length) < 0) {
        return -1;
    }
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 0;
}

// 单字符追加复用通用的缓冲区管理
static int builder_append_char(string_builder_t *builder, char value) {
    return builder_append(builder, &value, 1);
}

// 输出带双引号的 JSON 字符串，转义引号、反斜杠和控制字符，普通 UTF-8 字节原样复制
// 与 protocol.c 的行协议转义不同；这里也不负责校验整段 UTF-8 是否有效
static int builder_append_json_string(string_builder_t *builder, const char *text) {
    if (builder_append_char(builder, '"') < 0) {
        return -1;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        char escaped[7];
        size_t escaped_length = 0;
        switch (*cursor) {
            case '"': memcpy(escaped, "\\\"", 2); escaped_length = 2; break;
            case '\\': memcpy(escaped, "\\\\", 2); escaped_length = 2; break;
            case '\b': memcpy(escaped, "\\b", 2); escaped_length = 2; break;
            case '\f': memcpy(escaped, "\\f", 2); escaped_length = 2; break;
            case '\n': memcpy(escaped, "\\n", 2); escaped_length = 2; break;
            case '\r': memcpy(escaped, "\\r", 2); escaped_length = 2; break;
            case '\t': memcpy(escaped, "\\t", 2); escaped_length = 2; break;
            default:
                if (*cursor < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                    escaped_length = 6;
                } else {
                    escaped[0] = (char)*cursor;
                    escaped_length = 1;
                }
                break;
        }
        if (builder_append(builder, escaped, escaped_length) < 0) {
            return -1;
        }
    }
    return builder_append_char(builder, '"');
}

// 忽略大小写查找标记，用于识别 HTTP 头中的 chunked 等文本
static int contains_case_insensitive(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) return 1;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (strncasecmp(cursor, needle, needle_length) == 0) return 1;
    }
    return 0;
}

// 将内存上下文转换成 messages 数组，不读取磁盘；成功后缓冲区交给调用方释放
static int build_request_body(const session_t *session, char **body, size_t *body_length) {
    string_builder_t builder = {0};
    if (builder_append(&builder, "{\"messages\":[", 13) < 0) {
        goto fail;
    }
    for (size_t index = 0; index < session_count(session); ++index) {
        const session_message_t *message = session_message_at(session, index);
        if (index > 0 && builder_append_char(&builder, ',') < 0) {
            goto fail;
        }
        if (builder_append(&builder, "{\"role\":", strlen("{\"role\":")) < 0 ||
            builder_append_json_string(&builder, message->role) < 0 ||
            builder_append(&builder, ",\"content\":", strlen(",\"content\":")) < 0 ||
            builder_append_json_string(&builder, message->content) < 0 ||
            builder_append_char(&builder, '}') < 0) {
            goto fail;
        }
    }
    // 启用 SSE 流式输出、限制生成长度并关闭思考输出；512 不代表输入上下文预算
    // 当前请求直接写入 512，未使用上方的 AI_MAX_TOKENS 宏
    const char suffix[] = "],\"stream\":true,\"max_tokens\":512,"
                          "\"chat_template_kwargs\":{\"enable_thinking\":false}}";
    if (builder_append(&builder, suffix, strlen(suffix)) < 0) {
        goto fail;
    }
    *body = builder.data;
    *body_length = builder.length;
    return 0;

fail:
    free(builder.data);
    return -1;
}

// 完整发送 HTTP 字节，处理短发送和 EINTR；不使用项目自己的 Q/DATA 协议
static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

// 解析主机与端口，依次尝试可用地址；AF_UNSPEC 允许解析 IPv4 或 IPv6
static int connect_backend(const char *host, unsigned short port) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        errno = ECONNREFUSED;
        return -1;
    }

    int socket_fd = -1;
    for (struct addrinfo *entry = results; entry != NULL; entry = entry->ai_next) {
        socket_fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        // 设置 socket 收发等待超时，不是整轮生成必须在 120 秒内完成
        struct timeval timeout = {.tv_sec = 120, .tv_usec = 0};
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(socket_fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(results);
    return socket_fd;
}

// 读到空行 CRLF CRLF 为止，再提取状态码、传输编码和 Content-Length
static int read_headers(int fd, char *headers, size_t capacity, int *status,
                        int *chunked, size_t *content_length) {
    size_t used = 0;
    while (used + 1 < capacity) {
        // fd 是已连接模型的 socket 描述符；逐字节读头，避免多读到正文
        ssize_t result = recv(fd, headers + used, 1, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        ++used;
        headers[used] = '\0';
        if (used >= 4 && memcmp(headers + used - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    if (used + 1 >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    // %*s 跳过 HTTP 版本，只把状态码写入 status 指向的变量
    if (sscanf(headers, "HTTP/%*s %d", status) != 1) {
        errno = EPROTO;
        return -1;
    }
    *chunked = 0;
    *content_length = 0;
    char *cursor = headers;
    while ((cursor = strstr(cursor, "\r\n")) != NULL) {
        cursor += 2;
        if (*cursor == '\r' && cursor[1] == '\n') {
            break;
        }
        if (strncasecmp(cursor, "Transfer-Encoding:", 18) == 0 &&
            contains_case_insensitive(cursor + 18, "chunked")) {
            *chunked = 1;
        }
        if (strncasecmp(cursor, "Content-Length:", 15) == 0) {
            *content_length = (size_t)strtoull(cursor + 15, NULL, 10);
        }
    }
    return 0;
}

// 正文读取状态：chunked 表示 HTTP 分块编码，finished 表示读到结束
// remaining 用于当前块或定长正文的剩余字节；content_remaining 当前保存初始正文长度
typedef struct {
    int fd;
    int chunked;
    int finished;
    size_t remaining;
    size_t content_remaining;
} body_reader_t;

// 读取 HTTP chunk 长度行并去掉 CRLF；这里读的是编码元信息，不是 SSE 回答正文
static int read_body_line(body_reader_t *reader, char *line, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char byte;
        ssize_t result = recv(reader->fd, &byte, 1, 0);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        if (byte == '\n') {
            line[used] = '\0';
            if (used > 0 && line[used - 1] == '\r') line[--used] = '\0';
            return 0;
        }
        line[used++] = byte;
    }
    errno = EMSGSIZE;
    return -1;
}

// 向上层返回正文片段，隐藏 HTTP chunk 长度和分隔符；正值是字节数，0 是结束，负值是错误
static ssize_t body_read(body_reader_t *reader, char *buffer, size_t capacity) {
    if (reader->finished) return 0;
    // 非分块路径按 remaining 限制单次读取；本实现最终也会通过连接 EOF 判定结束
    // content_remaining 没有逐次递减，不能把它理解成独立维护的剩余计数
    if (!reader->chunked) {
        if (reader->content_remaining == 0 && reader->remaining != 0) {
            reader->finished = 1;
            return 0;
        }
        size_t wanted = capacity;
        if (reader->remaining != 0 && wanted > reader->remaining) wanted = reader->remaining;
        ssize_t result = recv(reader->fd, buffer, wanted, 0);
        if (result == 0) reader->finished = 1;
        if (result > 0 && reader->remaining != 0) reader->remaining -= (size_t)result;
        return result;
    }

    // 当前块已读完时，先读下一块的十六进制长度；长度为 0 表示分块正文结束
    if (reader->remaining == 0) {
        char size_line[64];
        if (read_body_line(reader, size_line, sizeof(size_line)) < 0) return -1;
        char *end = NULL;
        unsigned long long size = strtoull(size_line, &end, 16);
        if (end == size_line) {
            errno = EPROTO;
            return -1;
        }
        if (size == 0) {
            reader->finished = 1;
            return 0;
        }
        reader->remaining = (size_t)size;
    }

    // 一次最多读当前块的剩余部分，不要求一整个 HTTP 块放进 buffer
    size_t wanted = reader->remaining < capacity ? reader->remaining : capacity;
    // 这里面只会读需要的字节数量
    ssize_t result = recv(reader->fd, buffer, wanted, 0);
    if (result <= 0) return result;
    reader->remaining -= (size_t)result;
    if (reader->remaining == 0) {
        // 当前块读完后消费它后面的 CRLF，不把分隔符交给 SSE 解析器
        char crlf[2];
        if (recv(reader->fd, crlf, 2, MSG_WAITALL) != 2 || crlf[0] != '\r' || crlf[1] != '\n') {
            errno = EPROTO;
            return -1;
        }
    }
    return result;
}

// 将码点编码成 1～4 字节 UTF-8；当前实现没有处理 JSON 的代理项配对
static int append_utf8(string_builder_t *builder, unsigned int codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint; length = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f)); length = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f)); length = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = (char)(0xf0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (codepoint & 0x3f)); length = 4;
    } else {
        errno = EPROTO;
        return -1;
    }
    return builder_append(builder, bytes, length);
}

// 简化的字符串字段提取：搜索键名并解析字符串，不是完整的 JSON 语法解析器
// 返回 0 表示提取成功，1 表示未找到字符串字段，-1 表示解析或容量错误
static int extract_json_string(const char *json, const char *key,
                               char *output, size_t capacity) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *cursor = strstr(json, needle);
    if (cursor == NULL) return 1;
    cursor = strchr(cursor + strlen(needle), ':');
    if (cursor == NULL) return -1;
    ++cursor;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor != '"') return 1;
    ++cursor;
    // 跳过开头引号后，收集正文，遇到结束引号停止；反斜杠进入转义解析
    string_builder_t builder = {0};
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char raw_byte = (unsigned char)*cursor++;
        if (raw_byte != '\\') {
            if (builder_append(&builder, (const char *)&raw_byte, 1) < 0) {
                free(builder.data);
                return -1;
            }
            continue;
        }

        unsigned int codepoint;
        if (*cursor == '\0') {
            free(builder.data);
            return -1;
        }
        {
            switch (*cursor++) {
                case '"': codepoint = '"'; break;
                case '\\': codepoint = '\\'; break;
                case '/': codepoint = '/'; break;
                case 'b': codepoint = '\b'; break;
                case 'f': codepoint = '\f'; break;
                case 'n': codepoint = '\n'; break;
                case 'r': codepoint = '\r'; break;
                case 't': codepoint = '\t'; break;
                case 'u': {
                    // 读取四位十六进制 Unicode 转义，再由 append_utf8 转为文本字节
                    char hex[5] = {cursor[0], cursor[1], cursor[2], cursor[3], '\0'};
                    char *end = NULL;
                    codepoint = (unsigned int)strtoul(hex, &end, 16);
                    if (end != hex + 4) { free(builder.data); return -1; }
                    cursor += 4;
                    break;
                }
                default: free(builder.data); return -1;
            }
        }
        if (append_utf8(&builder, codepoint) < 0) {
            free(builder.data);
            return -1;
        }
    }
    if (*cursor != '"' || builder.length + 1 > capacity) {
        free(builder.data);
        return -1;
    }
    // 复制正文及结束符；现有空字符串路径可能使 builder.data 为 NULL，仍待修复
    memcpy(output, builder.data, builder.length + 1);
    free(builder.data);
    return 0;
}

// 处理一条 SSE 数据行：忽略其他行，识别 [DONE]、模型错误和回答 content
static int process_sse_line(const char *line, ai_chunk_callback callback,
                           void *userdata, int *done, char *error, size_t error_capacity) {
    if (strncmp(line, "data:", 5) != 0) return 0;
    const char *payload = line + 5;
    while (*payload == ' ') ++payload;
    // 设置调用方的结束标志；数据连接关闭但没收到此标记，会被认为是流提前结束
    if (strcmp(payload, "[DONE]") == 0) {
        *done = 1;
        return 0;
    }
    if (strstr(payload, "\"error\"") != NULL) {
        char message[AI_MAX_ERROR];
        if (extract_json_string(payload, "message", message, sizeof(message)) == 0) {
            set_error(error, error_capacity, "%s", message);
        } else {
            set_error(error, error_capacity, "llama-server returned an error");
        }
        return -1;
    }
    char content[AI_MAX_SSE_LINE];
    // line里面存储的是json文本，所以直接找到content键值对应的字符串值，放入缓冲区
    int result = extract_json_string(payload, "content", content, sizeof(content));
    if (result < 0) {
        set_error(error, error_capacity, "invalid content in model stream");
        return -1;
    }
    // 字段不存在时略过；非空正文交给回调，userdata 原样传回（服务端传入的是 &stream）
    // 回调非零表示上层处理失败，这里统一报告为客户端断开，实际也可能是内存分配失败
    if (result == 0 && content[0] != '\0' && callback(content, strlen(content), userdata) != 0) {
        set_error(error, error_capacity, "client disconnected during model stream");
        return -1;
    }
        return 0;
}

// 对外入口：构造请求、连接模型、解析 HTTP/SSE、逐块回调，最后统一释放资源
// 本函数同步执行；并发客户端由 server.c 的独立子进程分别调用
int ai_client_stream(const ai_client_config_t *config, const session_t *session,
                     ai_chunk_callback callback, void *userdata,
                     char *error_buffer, size_t error_capacity) {
    if (config == NULL || session == NULL || callback == NULL ||
        config->host == NULL || config->path == NULL) {
        set_error(error_buffer, error_capacity, "invalid AI client configuration");
        errno = EINVAL;
        return -1;
    }

    char *body = NULL;
    size_t body_length = 0;
    // 将上下文构造成内存中的 JSON 请求正文，不生成 JSON 文件
    if (build_request_body(session, &body, &body_length) < 0) {
        set_error(error_buffer, error_capacity, "cannot build model request");
        return -1;
    }
    // 主动连接模型所在的网络端口
    int socket_fd = connect_backend(config->host, config->port);
    if (socket_fd < 0) {
        set_error(error_buffer, error_capacity, "cannot connect to llama-server at %s:%u",
                  config->host, (unsigned int)config->port);
        free(body);
        return -1;
    }

    // HTTP 头与正文分开发送；Content-Length 是正文的字节数，Accept 声明接收 SSE
    // Connection: close 要求本次响应后关闭连接，每次模型请求单独建立连接
    char header[1024];
    int header_length = snprintf(header, sizeof(header),
                                 "POST %s HTTP/1.1\r\n"
                                 "Host: %s:%u\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Accept: text/event-stream\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 config->path, config->host, (unsigned int)config->port,
                                 body_length);
    int result = -1;
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        send_all(socket_fd, header, (size_t)header_length) < 0 ||
        send_all(socket_fd, body, body_length) < 0) {
        set_error(error_buffer, error_capacity, "failed to send model request");
        goto cleanup;
    }


    char headers[AI_MAX_HEADERS];
    int status = 0;
    int chunked = 0;
    size_t content_length = 0;
    // 阅读模型返回的http响应头和返回状态
    if (read_headers(socket_fd, headers, sizeof(headers), &status, &chunked, &content_length) < 0) {
        set_error(error_buffer, error_capacity, "invalid response from llama-server");
        goto cleanup;
    }
    // 只接受 2xx；当前失败路径仅报告状态码，没有读取模型的详细错误正文
    if (status < 200 || status >= 300) {
        set_error(error_buffer, error_capacity, "llama-server HTTP status %d", status);
        goto cleanup;
    }

    // 初始化正文读取状态；HTTP chunk 边界不一定与 SSE 行边界重合
    body_reader_t reader = {
        .fd = socket_fd,
        .chunked = chunked,
        .remaining = chunked ? 0 : content_length,
        .content_remaining = content_length,
    };
    // body_buffer 保存每次网络读取的片段，line 跨读取累计一整条 SSE 行
    char body_buffer[4096];
    char line[AI_MAX_SSE_LINE];
    size_t line_length = 0;
    int done = 0;
    while (!done) {
        // bytes 是本次实际读到的字节数；一条 SSE 行可能分散在多次读取中
        ssize_t bytes = body_read(&reader, body_buffer, sizeof(body_buffer));
        if (bytes < 0) {
            set_error(error_buffer, error_capacity, "error reading llama-server stream");
            goto cleanup;
        }
        if (bytes == 0) break;
        for (ssize_t index = 0; index < bytes; ++index) {
            char byte = body_buffer[index];
            // 读到换行才能处理完整 SSE 行；同时兼容 CRLF 并保留未完成行到下一次读取
            if (byte == '\n') {
                line[line_length] = '\0';
                if (line_length > 0 && line[line_length - 1] == '\r') {
                    line[--line_length] = '\0';
                }
                if (process_sse_line(line, callback, userdata, &done,
                                     error_buffer, error_capacity) < 0) {
                    goto cleanup;
                }
                line_length = 0;
            } else if (line_length + 1 < sizeof(line)) {
                // 这个就是把一行的内容放在一个line变量里面
                line[line_length++] = byte;
            } else {
                set_error(error_buffer, error_capacity, "model SSE line is too long");
                goto cleanup;
            }
        }
    }
    // 仅连接结束不算成功，必须收到模型的 [DONE] 才确认流完整
    if (!done) {
        set_error(error_buffer, error_capacity, "llama-server stream ended before [DONE]");
        goto cleanup;
    }
    result = 0;

cleanup:
    // 所有连接建立后的失败路径都跳到这里；回调收到的文本只在调用期间有效
    // 回答的累计与会话保存由 server.c 完成，本模块只释放 HTTP 请求正文和连接
    close(socket_fd);
    free(body);
    return result;
}
