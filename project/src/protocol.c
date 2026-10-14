#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// TCP 是字节流，本模块用真正的换行分隔帧；正文中的换行和反斜杠需要转义
// 以下收发函数中的 fd 都是连接描述符，不是进程编号或端口号

// 循环发送指定长度的数据；成功返回 0，失败返回 -1，不负责添加帧边界
int protocol_send_all(int fd, const void *buffer, size_t length) {
    // 转成字节指针，便于根据已发送长度计算下一次发送的起点
    const char *bytes = (const char *)buffer;
    size_t sent = 0;

    while (sent < length) {
        // send 一次可能只发送部分数据；从 bytes + sent 开始发送剩余部分
        // MSG_NOSIGNAL 避免对端断开时触发 SIGPIPE，错误仍通过返回值报告
        ssize_t result = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                // 被信号打断时重试，尚未发送的数据位置不变
                continue;
            }
            return -1;
        }
        if (result == 0) {
            // 没有发送进展则结束，避免循环永远停在同一位置
            return -1;
        }
        sent += (size_t)result;
    }
    // 成功表示全部字节已交给本地网络栈，不表示对端已处理完消息
    return 0;
}

// 读取一帧，不把结尾换行放进 buffer；capacity 包含字符串结束符的空间
// 返回 1 表示读到内容，0 表示未读到内容就遇到 EOF，-1 是错误，-2 是行超长
int protocol_read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    int overflow = 0;

    if (buffer == NULL || capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    // 每次读取一个字节，不会越过当前帧而消费下一帧的数据
    for (;;) {
        char byte;
        ssize_t result = recv(fd, &byte, 1, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            if (used == 0 && !overflow) {
                return 0;
            }
            // 已读到部分内容时，当前实现仍将 EOF 前的内容作为一行交给调用方
            break;
        }
        // 读到一行之后就结束
        if (byte == '\n') {
            break;
        }
        if (used + 1 < capacity) {
            buffer[used++] = byte;
        } else {
            // 超长后继续读并丢弃到行末，确保下一次调用从下一帧开始
            overflow = 1;
        }
    }

    buffer[used] = '\0';
    // 兼容 CRLF 行结束符：换行已消费，再去掉末尾可能存在的回车
    if (used > 0 && buffer[used - 1] == '\r') {
        buffer[--used] = '\0';
    }
    return overflow ? -2 : 1;
}

// 将正文中的反斜杠、换行、回车编码为两个普通字符，避免正文破坏行帧边界
// 按字节复制其他内容，因此有效 UTF-8 正文不会被重新编码；这不是 JSON 转义
int protocol_escape(const char *input, char *output, size_t capacity) {
    size_t used = 0;

    if (input == NULL || output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        const char *replacement = NULL;
        // 普通字节也临时组成以 0 结束的字符串，统一使用 strlen 和 memcpy
        char literal[2] = {0, 0};

        switch (*cursor) {
            case '\\': replacement = "\\\\"; break;
            case '\n':  replacement = "\\n"; break;
            case '\r':  replacement = "\\r"; break;
            default:
                literal[0] = (char)*cursor;
                replacement = literal;
                break;
        }

        size_t length = strlen(replacement);
        // 转义可能使正文变长；额外的 1 是为最终的字符串结束符预留空间
        if (used + length + 1 > capacity) {
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(output + used, replacement, length);
        used += length;
    }

    output[used] = '\0';
    return 0;
}

// 还原协议正文；只接受本协议定义的三种转义，未知或缺失的转义字符返回 EPROTO
int protocol_unescape(const char *input, char *output, size_t capacity) {
    size_t used = 0;

    if (input == NULL || output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t index = 0; input[index] != '\0'; ++index) {
        char value = input[index];
        if (value == '\\') {
            // 反斜杠与后面的字符共同表示一个原始字符，所以先推进 index
            char escaped = input[++index];
            if (escaped == '\0') {
                errno = EPROTO;
                return -1;
            }
            switch (escaped) {
                case '\\': value = '\\'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                default:
                    errno = EPROTO;
                    return -1;
            }
        }

        // 当前字符加上结束符必须都能放进输出缓冲区
        if (used + 1 >= capacity) {
            errno = EMSGSIZE;
            return -1;
        }
        output[used++] = value;
    }

    output[used] = '\0';
    return 0;
}

// 新版将此接口公开：既能发 Q/DATA，也能发 OPENED 和 SESSION 等会话帧
// kind 可包含受控元数据（例如 OPENED 3），自由文本放在会被转义的 payload 中
// 本函数只发送一帧，不自动分块；长正文应使用 protocol_send_text
int protocol_send_frame(int fd, const char *kind, const char *payload) {
    char escaped[PROTOCOL_MAX_LINE];
    char frame[PROTOCOL_MAX_LINE];

    // 处理转义字符，将文本内容中本身带有的 \n 处理成 \\n 在客户端里面处理的时候能够将正文内容当成一行来处理
    if (protocol_escape(payload, escaped, sizeof(escaped)) < 0) {
        return -1;
    }
    // 帧格式为 kind + 空格 + 转义正文 + 真正的换行；kind 本身不进行转义
    int length = snprintf(frame, sizeof(frame), "%s %s\n", kind, escaped);
    // snprintf 返回期望写入的长度；达到容量说明内容被截断，不能发送残缺帧
    if (length < 0 || (size_t)length >= sizeof(frame)) {
        errno = EMSGSIZE;
        return -1;
    }
    // 真正执行数据发送的函数，里面的send执行数据发送
    return protocol_send_all(fd, frame, (size_t)length);
}

// 以下函数是常用帧的包装，调用方不需要自己拼接协议类型
// 发送问题：正文可以包含换行，由通用帧接口转义
int protocol_send_query(int fd, const char *text) {
    return protocol_send_frame(fd, "Q", text);
}

// 无正文控制帧直接发送固定文本；sizeof - 1 排除 C 字符串结束符
int protocol_send_quit(int fd) {
    static const char frame[] = "QUIT\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

// 通知客户端本轮回复开始，后面可以接收 DATA
int protocol_send_begin(int fd) {
    static const char frame[] = "BEGIN\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

// 发送单个回答帧，不会拆分；新版服务端转发长文本时使用 send_text(..., "DATA", ...)
int protocol_send_data(int fd, const char *text) {
    return protocol_send_frame(fd, "DATA", text);
}

// 通知客户端本轮完成；是否已经保存成功由服务端业务逻辑决定
int protocol_send_end(int fd) {
    static const char frame[] = "END\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

// 错误描述同样需要转义，避免其中的换行被当作新帧
int protocol_send_error(int fd, const char *text) {
    return protocol_send_frame(fd, "ERROR", text);
}

// 新版长文本接口：回答用 DATA，历史正文用 TEXT，拆成同类型的多个帧
// 每块最多 2048 字节，最坏转义后约 4096 字节，为常用帧类型和边界留出空间
// 输入必须是以 0 结束的字符串；空文本不会发送任何帧；消息起止标记由调用方发送
int protocol_send_text(int fd, const char *kind, const char *text) {
    // length 是总字节数，offset 是已发送的正文位置，不是字符数或 token 数
    size_t length = strlen(text), offset = 0;
    while (offset < length) {
        size_t count = length - offset;
        if (count > 2048) count = 2048;
        // 检查下一块的起点：UTF-8 后续字节的高两位为 10，即 byte & 0xc0 == 0x80
        // 如果切在字符中间，就缩小当前块，把整个字符留给下一块
        // offset + count < length 避免访问正文之外；这里只调整边界，不校验 UTF-8
        while (offset + count < length && count > 0 &&
               ((unsigned char)text[offset + count] & 0xc0) == 0x80) --count;
        // 异常字节可能使 count 退到 0；兜底保证循环前进，不能保证异常文本字符完整
        if (!count) count = length - offset > 2048 ? 2048 : length - offset;
        // 多出的 1 字节放字符串结束符，以便交给 send_frame 进行转义和拼帧
        char chunk[2049];
        memcpy(chunk, text + offset, count); chunk[count] = 0;
        if (protocol_send_frame(fd, kind, chunk) < 0) return -1;
        // 只有当前块发送成功才推进；失败即返回，已经发送的块无法撤回
        offset += count;
    }
    return 0;
}
