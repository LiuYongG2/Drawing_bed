#include "http_conn.h"
//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";
//类的静态成员变量类外初始化
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

http_conn::http_conn(){

}
http_conn::~http_conn(){

}

void http_conn::init(int socket, char* root){
    init();
    m_socket = socket;
    server_root = root;
    addfd(m_epollfd, socket, EPOLLIN);
    m_user_count++;
}
void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_keep_alive = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bytes_to_send = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

void http_conn::addfd(int epfd, int fd, uint32_t events){
    epoll_event event;
    event.events = events;
    event.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
}

void http_conn::read_once(){
     data_read = read( m_socket, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx);
     if(data_read == -1){
        printf("read error");
        exit(1);
     }
     else if(data_read == 0){
        printf("客户端断开连接\n");
        removefd(m_epollfd, m_socket);
     }
     m_read_idx += data_read;
     printf("--------------------------\n");
     write(1, m_read_buf, m_read_idx);
     printf("--------------------------\n");
     process();

}

http_conn::HTTP_CODE http_conn::process_read()
{
    //从状态机的初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    while ((line_status = parse_line()) == LINE_OK)
    {
        text = get_line();//当前需要处理的这一行的字符串
        m_start_line = m_checked_idx;
        //printf("%s\n", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    m_check_state = CHECK_STATE_HEADER;
    printf("--m_method = %d, m_url = %s, m_version = %s\n", m_method, m_url, m_version);
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_keep_alive = true;
            printf("Connection: keep-alive\n");
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
        printf("Content-length: %d\n", m_content_length);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
        printf("Host: %s\n", text);
    }
    else
    {
        //printf("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    printf("HTTP 请求分析完毕，开始处理\n");
    strcpy(m_real_file, server_root);
    int len = strlen(server_root);
    //printf("m_url:%s\n", m_url);
    //const char *p = strrchr(m_url, '/');
    if(strlen(m_url) == 1 && m_url[0] == '/'){
        strncpy(m_real_file + len, "/index.html", FILENAME_LEN - len - 1);
    }
    else{
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }
    printf("m_real_file = %s\n", m_real_file);
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;//stat返回-1表示文件不存在
    if (!(m_file_stat.st_mode & S_IROTH))//判断其他人是否有读权限
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))//判断请求的文件是否为文件夹
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    close(fd);
    return FILE_REQUEST;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    do_write();
    //modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    printf("request:%s", m_write_buf);

    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_content_type() && add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    char file_type[32];
    char* p = strrchr(m_real_file, '.');
    printf("----------%s\n", p);
    strcpy(file_type, p);
    if(strlen(file_type) == 0){
        return false;
    }
    if(strcasecmp(file_type, ".jpg") == 0)
        strcpy(file_type, "image/jpeg; charset=utf-8");
    else if(strcasecmp(file_type, ".gif") == 0)
        strcpy(file_type, "image/gif; charset=utf-8");
    else if(strcasecmp(file_type, ".png") == 0)
        strcpy(file_type, "image/png; charset=utf-8");
    else
        strcpy(file_type, "text/html; charset=utf-8");
    return add_response("Content-Type:%s\r\n", file_type);
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_keep_alive == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_socket != -1))
    {
        printf("close %d\n", m_socket);
        removefd(m_epollfd, m_socket);
        m_socket = -1;
        m_user_count--;
    }
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

void http_conn::removefd(int epollfd, int fd)
{
    int ret = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    if(ret == -1){
        perror("epoll_ctl error");
        exit(1);
    }
    printf("fd = %d, 已解除监听\n", fd);
    close(fd);
}

bool http_conn::do_write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        init();
        return true;
    }
    temp = writev(m_socket, m_iv, m_iv_count);
    return true;
}