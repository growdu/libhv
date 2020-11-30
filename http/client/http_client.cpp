#include "http_client.h"

#include <mutex>

#ifdef WITH_CURL
#include "curl/curl.h"
#endif

#include "herr.h"
#include "hstring.h"
#include "hsocket.h"
#include "hssl.h"
#include "HttpParser.h"

// for async
#include "hthread.h"
#include "hloop.h"
struct http_client_s {
    std::string  host;
    int          port;
    int          https;
    int          timeout; // s
    http_headers headers;
//private:
#ifdef WITH_CURL
    CURL* curl;
#endif
    int fd;
    // for sync
    hssl_t          ssl;
    HttpParserPtr   parser;
    // for async
    std::mutex  mutex_;
    hthread_t   thread_;
    hloop_t*    loop_;

    http_client_s() {
        host = LOCALHOST;
        port = DEFAULT_HTTP_PORT;
        https = 0;
        timeout = DEFAULT_HTTP_TIMEOUT;
#ifdef WITH_CURL
        curl = NULL;
#endif
        fd = -1;
        ssl = NULL;

        thread_ = 0;
        loop_ = NULL;
    }

    ~http_client_s() {
        Close();
    }

    void Close() {
        if (loop_) {
            hloop_stop(loop_);
            loop_ = NULL;
        }
        if (thread_) {
            hthread_join(thread_);
            thread_ = 0;
        }
#ifdef WITH_CURL
        if (curl) {
            curl_easy_cleanup(curl);
            curl = NULL;
        }
#endif
        if (ssl) {
            hssl_free(ssl);
            ssl = NULL;
        }
        if (fd > 0) {
            closesocket(fd);
            fd = -1;
        }
    }
};

static int __http_client_send(http_client_t* cli, HttpRequest* req, HttpResponse* resp);

http_client_t* http_client_new(const char* host, int port, int https) {
    http_client_t* cli = new http_client_t;
    if (host) cli->host = host;
    cli->port = port;
    cli->https = https;
    cli->headers["Connection"] = "keep-alive";
    return cli;
}

int http_client_del(http_client_t* cli) {
    if (cli == NULL) return 0;
    delete cli;
    return 0;
}

int http_client_set_timeout(http_client_t* cli, int timeout) {
    cli->timeout = timeout;
    return 0;
}

int http_client_clear_headers(http_client_t* cli) {
    cli->headers.clear();
    return 0;
}

int http_client_set_header(http_client_t* cli, const char* key, const char* value) {
    cli->headers[key] = value;
    return 0;
}

int http_client_del_header(http_client_t* cli, const char* key) {
    auto iter = cli->headers.find(key);
    if (iter != cli->headers.end()) {
        cli->headers.erase(iter);
    }
    return 0;
}

const char* http_client_get_header(http_client_t* cli, const char* key) {
    auto iter = cli->headers.find(key);
    if (iter != cli->headers.end()) {
        return iter->second.c_str();
    }
    return NULL;
}

int http_client_send(http_client_t* cli, HttpRequest* req, HttpResponse* resp) {
    if (!cli || !req || !resp) return ERR_NULL_POINTER;

    if (req->url.empty() || *req->url.c_str() == '/') {
        req->host = cli->host;
        req->port = cli->port;
        req->https = cli->https;
    }

    if (req->timeout == 0) {
        req->timeout = cli->timeout;
    }

    for (auto& pair : cli->headers) {
        if (req->headers.find(pair.first) == req->headers.end()) {
            req->headers[pair.first] = pair.second;
        }
    }

    return __http_client_send(cli, req, resp);
}

int http_client_send(HttpRequest* req, HttpResponse* resp) {
    if (!req || !resp) return ERR_NULL_POINTER;

    http_client_t cli;
    return __http_client_send(&cli, req, resp);
}

#ifdef WITH_CURL
static size_t s_header_cb(char* buf, size_t size, size_t cnt, void* userdata) {
    if (buf == NULL || userdata == NULL)    return 0;

    HttpResponse* resp = (HttpResponse*)userdata;

    std::string str(buf);
    std::string::size_type pos = str.find_first_of(':');
    if (pos == std::string::npos) {
        if (strncmp(buf, "HTTP/", 5) == 0) {
            // status line
            //hlogd("%s", buf);
            int http_major,http_minor,status_code;
            if (buf[5] == '1') {
                // HTTP/1.1 200 OK\r\n
                sscanf(buf, "HTTP/%d.%d %d", &http_major, &http_minor, &status_code);
            }
            else if (buf[5] == '2') {
                // HTTP/2 200\r\n
                sscanf(buf, "HTTP/%d %d", &http_major, &status_code);
                http_minor = 0;
            }
            resp->http_major = http_major;
            resp->http_minor = http_minor;
            resp->status_code = (http_status)status_code;
        }
    }
    else {
        // headers
        std::string key = trim(str.substr(0, pos));
        std::string value = trim(str.substr(pos+1));
        resp->headers[key] = value;
    }
    return size*cnt;
}

static size_t s_body_cb(char *buf, size_t size, size_t cnt, void *userdata) {
    if (buf == NULL || userdata == NULL)    return 0;

    HttpResponse* resp = (HttpResponse*)userdata;
    resp->body.append(buf, size*cnt);
    return size*cnt;
}

int __http_client_send(http_client_t* cli, HttpRequest* req, HttpResponse* resp) {
    if (cli->curl == NULL) {
        cli->curl = curl_easy_init();
    }
    CURL* curl = cli->curl;

    // SSL
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);

    // http2
    if (req->http_major == 2) {
#if LIBCURL_VERSION_NUM < 0x073100 // 7.49.0
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2_0);
#else
        // No Connection: Upgrade
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
#endif
    }

    // TCP_NODELAY
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);

    // method
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_method_str(req->method));

    // url
    req->DumpUrl();
    curl_easy_setopt(curl, CURLOPT_URL, req->url.c_str());
    //hlogd("%s %s HTTP/%d.%d", http_method_str(req->method), req->url.c_str(), req->http_major, req->http_minor);

    // headers
    req->FillContentType();
    struct curl_slist *headers = NULL;
    for (auto& pair : req->headers) {
        std::string header = pair.first;
        header += ": ";
        header += pair.second;
        headers = curl_slist_append(headers, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // body
    //struct curl_httppost* httppost = NULL;
    //struct curl_httppost* lastpost = NULL;
    if (req->body.size() == 0) {
        req->DumpBody();
        /*
        if (req->body.size() == 0 &&
            req->content_type == MULTIPART_FORM_DATA) {
            for (auto& pair : req->mp) {
                if (pair.second.filename.size() != 0) {
                    curl_formadd(&httppost, &lastpost,
                            CURLFORM_COPYNAME, pair.first.c_str(),
                            CURLFORM_FILE, pair.second.filename.c_str(),
                            CURLFORM_END);
                }
                else if (pair.second.content.size() != 0) {
                    curl_formadd(&httppost, &lastpost,
                            CURLFORM_COPYNAME, pair.first.c_str(),
                            CURLFORM_COPYCONTENTS, pair.second.content.c_str(),
                            CURLFORM_END);
                }
            }
            if (httppost) {
                curl_easy_setopt(curl, CURLOPT_HTTPPOST, httppost);
            }
        }
        */
    }
    if (req->body.size() != 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, req->body.size());
    }

    if (cli->timeout > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, cli->timeout);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

    curl_easy_setopt(curl, CURLOPT_HEADER, 0);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, s_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);

    int ret = curl_easy_perform(curl);
    /*
    if (ret != 0) {
        hloge("curl error: %d: %s", ret, curl_easy_strerror((CURLcode)ret));
    }
    if (resp->body.length() != 0) {
        hlogd("[Response]\n%s", resp->body.c_str());
    }
    double total_time, name_time, conn_time, pre_time;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &name_time);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &conn_time);
    curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME, &pre_time);
    hlogd("TIME_INFO: %lf,%lf,%lf,%lf", total_time, name_time, conn_time, pre_time);
    */

    if (headers) {
        curl_slist_free_all(headers);
    }
    /*
    if (httppost) {
        curl_formfree(httppost);
    }
    */

    return ret;
}

const char* http_client_strerror(int errcode) {
    return curl_easy_strerror((CURLcode)errcode);
}
#else
static int __http_client_connect(http_client_t* cli, HttpRequest* req) {
    int blocktime = DEFAULT_CONNECT_TIMEOUT;
    if (req->timeout > 0) {
        blocktime = MIN(req->timeout*1000, blocktime);
    }
    req->ParseUrl();
    int connfd = ConnectTimeout(req->host.c_str(), req->port, blocktime);
    if (connfd < 0) {
        return connfd;
    }
    tcp_nodelay(connfd, 1);

    if (cli->https) {
        if (hssl_ctx_instance() == NULL) {
            hssl_ctx_init(NULL);
        }
        hssl_ctx_t ssl_ctx = hssl_ctx_instance();
        if (ssl_ctx == NULL) {
            closesocket(connfd);
            return ERR_INVALID_PROTOCOL;
        }
        cli->ssl = hssl_new(ssl_ctx, connfd);
        int ret = hssl_connect(cli->ssl);
        if (ret != 0) {
            fprintf(stderr, "SSL handshark failed: %d\n", ret);
            hssl_free(cli->ssl);
            cli->ssl = NULL;
            closesocket(connfd);
            return ret;
        }
    }

    if (cli->parser == NULL) {
        cli->parser = HttpParserPtr(HttpParser::New(HTTP_CLIENT, (http_version)req->http_major));
    }

    cli->fd = connfd;
    return 0;
}

int __http_client_send(http_client_t* cli, HttpRequest* req, HttpResponse* resp) {
    // connect -> send -> recv -> http_parser
    int err = 0;
    int timeout = cli->timeout;
    int connfd = cli->fd;

    time_t start_time = time(NULL);
    time_t cur_time;
    int fail_cnt = 0;
connect:
    if (connfd <= 0) {
        int ret = __http_client_connect(cli, req);
        if (ret != 0) {
            return ret;
        }
        connfd = cli->fd;
    }

    cli->parser->SubmitRequest(req);
    char recvbuf[1024] = {0};
    int total_nsend, nsend, nrecv;
    total_nsend = nsend = nrecv = 0;
send:
    char* data = NULL;
    size_t len  = 0;
    while (cli->parser->GetSendData(&data, &len)) {
        total_nsend = 0;
        while (1) {
            if (timeout > 0) {
                cur_time = time(NULL);
                if (cur_time - start_time >= timeout) {
                    return ERR_TASK_TIMEOUT;
                }
                so_sndtimeo(connfd, (timeout-(cur_time-start_time)) * 1000);
            }
            if (cli->https) {
                nsend = hssl_write(cli->ssl, data+total_nsend, len-total_nsend);
            }
            else {
                nsend = send(connfd, data+total_nsend, len-total_nsend, 0);
            }
            if (nsend <= 0) {
                if (++fail_cnt == 1) {
                    // maybe keep-alive timeout, try again
                    cli->Close();
                    goto connect;
                }
                else {
                    return socket_errno();
                }
            }
            total_nsend += nsend;
            if (total_nsend == len) {
                break;
            }
        }
    }
    cli->parser->InitResponse(resp);
recv:
    do {
        if (timeout > 0) {
            cur_time = time(NULL);
            if (cur_time - start_time >= timeout) {
                return ERR_TASK_TIMEOUT;
            }
            so_rcvtimeo(connfd, (timeout-(cur_time-start_time)) * 1000);
        }
        if (cli->https) {
            nrecv = hssl_read(cli->ssl, recvbuf, sizeof(recvbuf));
        }
        else {
            nrecv = recv(connfd, recvbuf, sizeof(recvbuf), 0);
        }
        if (nrecv <= 0) {
            return socket_errno();
        }
        int nparse = cli->parser->FeedRecvData(recvbuf, nrecv);
        if (nparse != nrecv) {
            return ERR_PARSE;
        }
    } while(!cli->parser->IsComplete());
    return err;
}

const char* http_client_strerror(int errcode) {
    return socket_strerror(errcode);
}
#endif

struct HttpContext {
    HttpRequestPtr  req;
    HttpResponsePtr resp;
    HttpParserPtr   parser;

    hio_t*          io;
    htimer_t*       timer;

    HttpResponseCallback cb;
    void*                userdata;

    HttpContext() {
        io = NULL;
        timer = NULL;
        cb = NULL;
        userdata = NULL;
    }

    ~HttpContext() {
        killTimer();
    }

    void closeIO() {
        if (io) {
            hio_close(io);
            io = NULL;
        }
    }

    void killTimer() {
        if (timer) {
            htimer_del(timer);
            timer = NULL;
        }
    }

    void callback(int state) {
        if (cb) {
            cb(state, req, resp, userdata);
            // NOTE: ensure cb only called once
            cb = NULL;
        }
    }

    void successCallback() {
        killTimer();
        callback(0);
    }

    void errorCallback(int error) {
        closeIO();
        callback(error);
    }
};

static void on_close(hio_t* io) {
    HttpContext* ctx = (HttpContext*)hevent_userdata(io);
    if (ctx) {
        int error = hio_error(io);
        ctx->callback(error);
        delete ctx;
        hevent_set_userdata(io, NULL);
    }
}

static void on_recv(hio_t* io, void* buf, int readbytes) {
    HttpContext* ctx = (HttpContext*)hevent_userdata(io);

    int nparse = ctx->parser->FeedRecvData((const char*)buf, readbytes);
    if (nparse != readbytes) {
        ctx->errorCallback(ERR_PARSE);
        return;
    }
    if (ctx->parser->IsComplete()) {
        ctx->successCallback();
        return;
    }
}

static void on_connect(hio_t* io) {
    HttpContext* ctx = (HttpContext*)hevent_userdata(io);

    ctx->parser = HttpParserPtr(HttpParser::New(HTTP_CLIENT, (http_version)ctx->req->http_major));
    ctx->parser->InitResponse(ctx->resp.get());
    ctx->parser->SubmitRequest(ctx->req.get());

    char* data = NULL;
    size_t len = 0;
    while (ctx->parser->GetSendData(&data, &len)) {
        hio_write(io, data, len);
    }

    hio_setcb_read(io, on_recv);
    hio_read(io);
}

static void on_timeout(htimer_t* timer) {
    HttpContext* ctx = (HttpContext*)hevent_userdata(timer);
    ctx->errorCallback(ERR_TASK_TIMEOUT);
}

static HTHREAD_ROUTINE(http_client_loop_thread) {
    hloop_t* loop = (hloop_t*)userdata;
    assert(loop != NULL);
    hloop_run(loop);
    return 0;
}

// hloop_new -> htread_create -> hloop_run ->
// hio_connect -> on_connect -> hio_write -> hio_read -> on_recv ->
// HttpResponseCallback -> on_close
static int __http_client_send_async(http_client_t* cli, HttpRequestPtr req, HttpResponsePtr resp,
        HttpResponseCallback cb, void* userdata) {
    sockaddr_u peeraddr;
    memset(&peeraddr, 0, sizeof(peeraddr));
    req->ParseUrl();
    int ret = sockaddr_set_ipport(&peeraddr, req->host.c_str(), req->port);
    if (ret != 0) {
        return ERR_INVALID_PARAM;
    }
    int connfd = socket(peeraddr.sa.sa_family, SOCK_STREAM, 0);
    if (connfd < 0) {
        return ERR_SOCKET;
    }

    cli->mutex_.lock();
    if (cli->loop_ == NULL) {
        cli->loop_ = hloop_new(HLOOP_FLAG_AUTO_FREE);
    }
    if (cli->thread_ == 0) {
        cli->thread_ = hthread_create(http_client_loop_thread, cli->loop_);
    }
    cli->mutex_.unlock();

    hio_t* connio = hio_get(cli->loop_, connfd);
    assert(connio != NULL);

    hio_set_peeraddr(connio, &peeraddr.sa, sockaddr_len(&peeraddr));
    hio_setcb_connect(connio, on_connect);
    hio_setcb_close(connio, on_close);

    // https
    if (req->https) {
        hio_enable_ssl(connio);
    }

    // new HttpContext
    // delete on_close
    HttpContext* ctx = new HttpContext;
    ctx->io = connio;
    ctx->req = req;
    ctx->resp = resp;
    ctx->cb = cb;
    ctx->userdata = userdata;
    hevent_set_userdata(connio, ctx);

    // timeout
    if (req->timeout != 0) {
        ctx->timer = htimer_add(cli->loop_, on_timeout, req->timeout * 1000, 1);
        assert(ctx->timer != NULL);
        hevent_set_userdata(ctx->timer, ctx);
    }

    return hio_connect(connio);
}

int http_client_send_async(http_client_t* cli, HttpRequestPtr req, HttpResponsePtr resp,
        HttpResponseCallback cb, void* userdata) {
    if (!cli || !req || !resp) return ERR_NULL_POINTER;

    if (req->url.empty() || *req->url.c_str() == '/') {
        req->host = cli->host;
        req->port = cli->port;
        req->https = cli->https;
    }

    if (req->timeout == 0) {
        req->timeout = cli->timeout;
    }

    for (auto& pair : cli->headers) {
        if (req->headers.find(pair.first) == req->headers.end()) {
            req->headers[pair.first] = pair.second;
        }
    }

    return __http_client_send_async(cli, req, resp, cb, userdata);
}

int http_client_send_async(HttpRequestPtr req, HttpResponsePtr resp,
        HttpResponseCallback cb, void* userdata) {
    if (!req || !resp) return ERR_NULL_POINTER;

    static http_client_t s_default_async_client;
    return __http_client_send_async(&s_default_async_client, req, resp, cb, userdata);
}
