/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <openssl/err.h>
#include "yoml-parser.h"
#include "h2o.h"
#include "h2o/configurator.h"
#include "h2o/http1.h"
#include "h2o/http2.h"
#include "h2o/serverutil.h"

struct listener_configurator_t {
    h2o_configurator_t super;
    size_t num_global_listeners;
    size_t num_host_listeners;
};

struct listener_ssl_config_t {
    H2O_VECTOR(h2o_iovec_t) hostnames;
    char *certificate_file;
    SSL_CTX *ctx;
};

struct listener_config_t {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    H2O_VECTOR(struct listener_ssl_config_t) ssl;
};

struct listener_ctx_t {
    h2o_context_t *ctx;
    SSL_CTX *ssl_ctx;
    h2o_socket_t *sock;
};

struct config_t {
    h2o_globalconf_t globalconf;
    int dry_run;
    struct {
        int *fds;
        char *bound_fd_map; /* has `num_fds` elements, set to 1 if fd[index] was bound to one of the listeners */
        size_t num_fds;
    } server_starter;
    struct listener_config_t **listeners;
    size_t num_listeners;
    struct passwd *running_user; /* NULL if not set */
    int max_connections;
    unsigned num_threads;
    pthread_t *thread_ids;
    struct {
        /* unused buffers exist to avoid false sharing of the cache line */
        char _unused1[32];
        int _num_connections; /* should use atomic functions to update the value */
        char _unused2[32];
    } state;
};

static unsigned long openssl_thread_id_callback(void)
{
    return (unsigned long)pthread_self();
}

static pthread_mutex_t *openssl_thread_locks;

static void openssl_thread_lock_callback(int mode, int n, const char *file, int line)
{
    if ((mode & CRYPTO_LOCK) != 0) {
        pthread_mutex_lock(openssl_thread_locks + n);
    } else if ((mode & CRYPTO_UNLOCK) != 0) {
        pthread_mutex_unlock(openssl_thread_locks + n);
    } else {
        assert(!"unexpected mode");
    }
}

static void init_openssl(void)
{
    static int ready = 0;
    if (! ready) {
        int nlocks = CRYPTO_num_locks(), i;
        openssl_thread_locks = h2o_mem_alloc(sizeof(*openssl_thread_locks) * nlocks);
        for (i = 0; i != nlocks; ++i)
            pthread_mutex_init(openssl_thread_locks + i, NULL);
        CRYPTO_set_locking_callback(openssl_thread_lock_callback);
        CRYPTO_set_id_callback(openssl_thread_id_callback);
        /* TODO [OpenSSL] set dynlock callbacks for better performance */
        SSL_load_error_strings();
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        ready = 1;
    }
}

static void setup_ecc_key(SSL_CTX *ssl_ctx)
{
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    SSL_CTX_set_tmp_ecdh(ssl_ctx, key);
    EC_KEY_free(key);
}

static int on_sni_callback(SSL *ssl, int *ad, void *arg)
{
    struct listener_config_t *listener = arg;
    const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    size_t ctx_index = 0;

    if (name != NULL) {
        size_t i, j, name_len = strlen(name);
        for (i = 0; i != listener->ssl.size; ++i) {
            struct listener_ssl_config_t *ssl_config = listener->ssl.entries + i;
            for (j = 0; j != ssl_config->hostnames.size; ++j) {
                if (h2o_lcstris(name, name_len, ssl_config->hostnames.entries[j].base, ssl_config->hostnames.entries[j].len)) {
                    ctx_index = i;
                    goto Found;
                }
            }
        }
        ctx_index = 0;
    Found:
        ;
    }

    if (SSL_get_SSL_CTX(ssl) != listener->ssl.entries[ctx_index].ctx)
        SSL_set_SSL_CTX(ssl, listener->ssl.entries[ctx_index].ctx);

    return SSL_TLSEXT_ERR_OK;
}

static void listener_setup_ssl_add_host(struct listener_ssl_config_t *ssl_config, h2o_iovec_t host)
{
    const char *host_end = memchr(host.base, ':', host.len);
    if (host_end == NULL)
        host_end = host.base + host.len;

    h2o_vector_reserve(NULL, (void*)&ssl_config->hostnames, sizeof(ssl_config->hostnames.entries[0]), ssl_config->hostnames.size + 1);
    ssl_config->hostnames.entries[ssl_config->hostnames.size++] = h2o_iovec_init(host.base, host_end - host.base);
}

static int listener_setup_ssl(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, const char *config_file, yoml_t *listen_config_node, yoml_t *ssl_config_node, struct listener_config_t *listener, int listener_is_new)
{
    SSL_CTX *ssl_ctx = NULL;
    yoml_t *certificate_file = NULL, *key_file = NULL, *minimum_version = NULL, *cipher_suite = NULL;
    long ssl_options = SSL_OP_ALL;

    if (! listener_is_new) {
        if (listener->ssl.size != 0 && ssl_config_node == NULL) {
            h2o_configurator_errprintf(cmd, config_file, listen_config_node, "cannot accept HTTP; already defined to accept HTTPS");
            return -1;
        }
        if (listener->ssl.size == 0 && ssl_config_node != NULL) {
            h2o_configurator_errprintf(cmd, config_file, ssl_config_node, "cannot accept HTTPS; already defined to accept HTTP");
            return -1;
        }
    }

    if (ssl_config_node == NULL)
        return 0;
    if (ssl_config_node->type != YOML_TYPE_MAPPING) {
        h2o_configurator_errprintf(cmd, config_file, ssl_config_node, "`ssl` is not a mapping");
        return -1;
    }

    { /* parse */
        size_t i;
        for (i = 0; i != ssl_config_node->data.sequence.size; ++i) {
            yoml_t *key = ssl_config_node->data.mapping.elements[i].key,
                *value = ssl_config_node->data.mapping.elements[i].value;
            /* obtain the target command */
            if (key->type != YOML_TYPE_SCALAR) {
                h2o_configurator_errprintf(NULL, config_file, key, "command must be a string");
                return -1;
            }
#define FETCH_PROPERTY(n, p) \
    if (strcmp(key->data.scalar, n) == 0) { \
        if (value->type != YOML_TYPE_SCALAR) { \
            h2o_configurator_errprintf(cmd, config_file, value, "property of `" n "` must be a string"); \
            return -1; \
        } \
        p = value; \
        continue; \
    } else
            FETCH_PROPERTY("certificate-file", certificate_file);
            FETCH_PROPERTY("key-file", key_file);
            FETCH_PROPERTY("minimum-version", minimum_version);
            FETCH_PROPERTY("cipher-suite", cipher_suite);
            h2o_configurator_errprintf(cmd, config_file, key, "unknown property: %s", key->data.scalar);
            return -1;
#undef FETCH_PROPERTY
        }
        if (certificate_file == NULL) {
            h2o_configurator_errprintf(cmd, config_file, ssl_config_node, "could not find mandatory property `certificate-file`");
            return -1;
        }
        if (key_file == NULL) {
            h2o_configurator_errprintf(cmd, config_file, ssl_config_node, "could not find mandatory property `key-file`");
            return -1;
        }
        if (minimum_version != NULL) {
#define MAP(tok, op) if (strcasecmp(minimum_version->data.scalar, tok) == 0) { ssl_options |= (op); goto VersionFound; }
            MAP("sslv2", 0);
            MAP("sslv3", SSL_OP_NO_SSLv2);
            MAP("tlsv1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
            MAP("tlsv1.1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1);
#ifdef SSL_OP_NO_TLSv1_1
            MAP("tlsv1.2", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#endif
#ifdef SSL_OP_NO_TLSv1_2
            MAP("tlsv1.3", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_NO_TLSv1_2);
#endif
#undef MAP
            h2o_configurator_errprintf(cmd, config_file, minimum_version, "unknown protocol version: %s", minimum_version->data.scalar);
        VersionFound:
            ;
        } else {
            /* default is >= TLSv1 */
            ssl_options |= SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
        }
    }

    /* add the host to the existing SSL config, if the certificate file is already registered */
    if (ctx->hostconf != NULL) {
        size_t i;
        for (i = 0; i != listener->ssl.size; ++i) {
            struct listener_ssl_config_t *ssl_config = listener->ssl.entries + i;
            if (strcmp(ssl_config->certificate_file, certificate_file->data.scalar) == 0) {
                listener_setup_ssl_add_host(ssl_config, ctx->hostconf->hostname);
                return 0;
            }
        }
    }

    /* setup */
    init_openssl();
    ssl_ctx = SSL_CTX_new(SSLv23_server_method());
    SSL_CTX_set_options(ssl_ctx, ssl_options);
    setup_ecc_key(ssl_ctx);
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, certificate_file->data.scalar) != 1) {
        h2o_configurator_errprintf(cmd, config_file, certificate_file, "failed to load certificate file:%s\n", certificate_file->data.scalar);
        ERR_print_errors_fp(stderr);
        goto Error;
    }
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file->data.scalar, SSL_FILETYPE_PEM) != 1) {
        h2o_configurator_errprintf(cmd, config_file, key_file, "failed to load private key file:%s\n", key_file->data.scalar);
        ERR_print_errors_fp(stderr);
        goto Error;
    }
    if (cipher_suite != NULL && SSL_CTX_set_cipher_list(ssl_ctx, cipher_suite->data.scalar) != 1) {
        h2o_configurator_errprintf(cmd, config_file, cipher_suite, "failed to setup SSL cipher suite\n");
        ERR_print_errors_fp(stderr);
        goto Error;
    }

    /* setup protocol negotiation methods */
#if H2O_USE_NPN
    h2o_ssl_register_npn_protocols(ssl_ctx, h2o_http2_npn_protocols);
#endif
#if H2O_USE_ALPN
    h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif

    /* set SNI callback to the first SSL context, when and only when it should be used */
    if (listener->ssl.size == 1) {
        SSL_CTX_set_tlsext_servername_callback(listener->ssl.entries[0].ctx, on_sni_callback);
        SSL_CTX_set_tlsext_servername_arg(listener->ssl.entries[0].ctx, listener);
    }

    { /* create a new entry in the SSL context list */
        struct listener_ssl_config_t *ssl_config;
        h2o_vector_reserve(NULL, (void*)&listener->ssl, sizeof(listener->ssl.entries[0]), listener->ssl.size + 1);
        ssl_config = listener->ssl.entries + listener->ssl.size++;
        memset(ssl_config, 0, sizeof(*ssl_config));
        if (ctx->hostconf != NULL) {
            listener_setup_ssl_add_host(ssl_config, ctx->hostconf->hostname);
        }
        ssl_config->ctx = ssl_ctx;
        ssl_config->certificate_file = h2o_strdup(NULL, certificate_file->data.scalar, SIZE_MAX).base;
    }

    return 0;

Error:
    if (ssl_ctx != NULL)
        SSL_CTX_free(ssl_ctx);
    return -1;
}

static struct listener_config_t *find_listener(struct config_t *conf, struct sockaddr *addr, socklen_t addrlen)
{
    size_t i;

    for (i = 0; i != conf->num_listeners; ++i) {
        struct listener_config_t *listener = conf->listeners[i];
        if (listener->addrlen == addrlen
            && h2o_socket_compare_address((void*)&listener->addr, addr) == 0)
            return listener;
    }

    return NULL;
}

static struct listener_config_t *add_listener(struct config_t *conf, int fd, struct sockaddr *addr, socklen_t addrlen)
{
    struct listener_config_t *listener = h2o_mem_alloc(sizeof(*listener));

    memcpy(&listener->addr, addr, addrlen);
    listener->fd = fd;
    listener->addrlen = addrlen;
    memset(&listener->ssl, 0, sizeof(listener->ssl));
    conf->listeners = h2o_mem_realloc(conf->listeners, sizeof(*conf->listeners) * (conf->num_listeners + 1));
    conf->listeners[conf->num_listeners++] = listener;

    return listener;
}

static int find_listener_from_server_starter(struct config_t *conf, struct sockaddr *addr)
{
    size_t i;

    assert(conf->server_starter.fds != NULL);
    assert(conf->server_starter.num_fds != 0);

    for (i = 0; i != conf->server_starter.num_fds; ++i) {
        struct sockaddr_storage sa;
        socklen_t salen = sizeof(sa);
        if (getsockname(conf->server_starter.fds[i], (void*)&sa, &salen) != 0) {
            fprintf(stderr, "could not get the socket address of fd %d given as $SERVER_STARTER_PORT\n", conf->server_starter.fds[i]);
            exit(EX_CONFIG);
        }
        if (h2o_socket_compare_address((void*)&sa, addr) == 0)
            goto Found;
    }
    /* not found */
    return -1;

Found:
    conf->server_starter.bound_fd_map[i] = 1;
    return conf->server_starter.fds[i];
}

static int open_unix_listener(h2o_configurator_command_t *cmd, const char *config_file, yoml_t *config_node, struct sockaddr_un *sun)
{
    struct stat st;
    int fd;

    /* remove existing socket file as suggested in #45 */
    if (lstat(sun->sun_path, &st) == 0) {
        if (S_ISSOCK(st.st_mode)) {
            unlink(sun->sun_path);
        } else {
            h2o_configurator_errprintf(cmd, config_file, config_node, "path:%s already exists and is not an unix socket.", sun->sun_path);
            return -1;
        }
    }
    /* add new listener */
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1
        || bind(fd, (void *)sun, sizeof(*sun)) != 0
        || listen(fd, SOMAXCONN) != 0) {
        if (fd != -1)
            close(fd);
        h2o_configurator_errprintf(NULL, config_file, config_node, "failed to listen to socket:%s: %s", sun->sun_path, strerror(errno));
        return -1;
    }

    return fd;
}

static int open_tcp_listener(h2o_configurator_command_t *cmd, const char *config_file, yoml_t *config_node, const char *hostname, const char *servname, int domain, int type, int protocol, struct sockaddr *addr, socklen_t addrlen)
{
    int fd;

    if ((fd = socket(domain, type, protocol)) == -1)
        goto Error;
    { /* set reuseaddr */
        int flag = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#ifdef TCP_DEFER_ACCEPT
    { /* set TCP_DEFER_ACCEPT */
        int flag = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#endif
#ifdef IPV6_V6ONLY
    /* set IPv6only */
    if (domain == AF_INET6) {
        int flag = 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#endif
    if (bind(fd, addr, addrlen) != 0)
        goto Error;
    if (listen(fd, SOMAXCONN) != 0)
        goto Error;

    return fd;

Error:
    if (fd != -1)
        close(fd);
    h2o_configurator_errprintf(NULL, config_file, config_node, "failed to listen to port %s:%s: %s", hostname != NULL ? hostname : "ANY", servname, strerror(errno));
    return -1;
}

static int on_config_listen(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, const char *config_file, yoml_t *config_node)
{
    struct listener_configurator_t *configurator = (void*)cmd->configurator;
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->globalconf);
    const char *hostname = NULL, *servname = NULL, *type = "tcp";
    yoml_t *ssl_config_node = NULL;

    if (ctx->hostconf == NULL)
        ++configurator->num_global_listeners;
    else
        ++configurator->num_host_listeners;

    /* fetch servname (and hostname) */
    switch (config_node->type) {
    case YOML_TYPE_SCALAR:
        servname = config_node->data.scalar;
        break;
    case YOML_TYPE_MAPPING:
        {
            yoml_t *t;
            if ((t = yoml_get(config_node, "host")) != NULL) {
                if (t->type != YOML_TYPE_SCALAR) {
                    h2o_configurator_errprintf(cmd, config_file, t, "`host` is not a string");
                    return -1;
                }
                hostname = t->data.scalar;
            }
            if ((t = yoml_get(config_node, "port")) == NULL) {
                h2o_configurator_errprintf(cmd, config_file, config_node, "cannot find mandatory property `port`");
                return -1;
            }
            if (t->type != YOML_TYPE_SCALAR) {
                h2o_configurator_errprintf(cmd, config_file, config_node, "`port` is not a string");
                return -1;
            }
            servname = t->data.scalar;
            if ((t = yoml_get(config_node, "type")) != NULL) {
                if (t->type != YOML_TYPE_SCALAR) {
                    h2o_configurator_errprintf(cmd, config_file, t, "`type` is not a string");
                    return -1;
                }
                type = t->data.scalar;
            }
            if ((t = yoml_get(config_node, "ssl")) != NULL)
                ssl_config_node = t;
        }
        break;
    default:
        h2o_configurator_errprintf(cmd, config_file, config_node, "value must be a string or a mapping (with keys: `port` and optionally `host`)");
        return -1;
    }

    if (strcmp(type, "unix") == 0) {

        /* unix socket */
        struct sockaddr_un sun;
        int listener_is_new;
        struct listener_config_t *listener;
        /* build sockaddr */
        if (strlen(servname) >= sizeof(sun.sun_path)) {
            h2o_configurator_errprintf(cmd, config_file, config_node, "path:%s is too long as a unix socket name", servname);
            return -1;
        }
        sun.sun_family = AF_UNIX;
        strcpy(sun.sun_path, servname);
        /* find existing listener or create a new one */
        listener_is_new = 0;
        if ((listener = find_listener(conf, (void*)&sun, sizeof(sun))) == NULL) {
            int fd;
            if (conf->server_starter.fds != NULL) {
                if ((fd = find_listener_from_server_starter(conf, (void*)&sun)) == -1) {
                    h2o_configurator_errprintf(cmd, config_file, config_node, "unix socket:%s is not being bound to the server\n", sun.sun_path);
                    return -1;
                }
            } else if (conf->dry_run) {
                fd = -1;
            } else {
                if ((fd = open_unix_listener(cmd, config_file, config_node, &sun)) == -1)
                    return -1;
            }
            listener = add_listener(conf, fd, (struct sockaddr*)&sun, sizeof(sun));
            listener_is_new = 1;
        }
        if (listener_setup_ssl(cmd, ctx, config_file, config_node, ssl_config_node, listener, listener_is_new) != 0)
            return -1;

    } else if (strcmp(type, "tcp") == 0) {

        /* TCP socket */
        struct addrinfo hints, *res, *ai;
        int error;
        /* call getaddrinfo */
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
        if ((error = getaddrinfo(hostname, servname, &hints, &res)) != 0) {
            h2o_configurator_errprintf(cmd, config_file, config_node, "failed to resolve the listening address: %s", gai_strerror(error));
            return -1;
        } else if (res == NULL) {
            h2o_configurator_errprintf(cmd, config_file, config_node, "failed to resolve the listening address: getaddrinfo returned an empty list");
            return -1;
        }
        /* listen to the returned addresses */
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            struct listener_config_t *listener = find_listener(conf, ai->ai_addr, ai->ai_addrlen);
            int listener_is_new = 0;
            if (listener == NULL) {
                int fd;
                if (conf->server_starter.fds != NULL) {
                    if ((fd = find_listener_from_server_starter(conf, ai->ai_addr)) == -1) {
                        h2o_configurator_errprintf(cmd, config_file, config_node, "tcp socket:%s:%s is not being bound to the server\n", hostname, servname);
                        return -1;
                    }
                } else if (conf->dry_run) {
                    fd = -1;
                } else {
                    if ((fd = open_tcp_listener(cmd, config_file, config_node, hostname, servname, ai->ai_family, ai->ai_socktype, ai->ai_protocol, ai->ai_addr, ai->ai_addrlen)) == -1)
                        return -1;
                }
                listener = add_listener(conf, fd, ai->ai_addr, ai->ai_addrlen);
                listener_is_new = 1;
            }
            if (listener_setup_ssl(cmd, ctx, config_file, config_node, ssl_config_node, listener, listener_is_new) != 0)
                return -1;
        }
        /* release res */
        freeaddrinfo(res);

    } else {

        h2o_configurator_errprintf(cmd, config_file, config_node, "unknown listen type: %s", type);
        return -1;

    }

    return 0;
}

static int on_config_listen_enter(h2o_configurator_t *_configurator, h2o_configurator_context_t *ctx, const char *file, yoml_t *node)
{
    struct listener_configurator_t *configurator = (void*)_configurator;

    /* bail-out unless at host-level */
    if (ctx->hostconf == NULL || ctx->pathconf != NULL)
        return 0;

    configurator->num_host_listeners = 0;
    return 0;
}

static int on_config_listen_exit(h2o_configurator_t *_configurator, h2o_configurator_context_t *ctx, const char *file, yoml_t *node)
{
    struct listener_configurator_t *configurator = (void*)_configurator;

    /* bail-out unless at host-level */
    if (ctx->hostconf == NULL || ctx->pathconf != NULL)
        return 0;

    if (configurator->num_host_listeners == 0 && configurator->num_global_listeners == 0) {
        h2o_configurator_errprintf(NULL, file, node, "mandatory configuration directive `listen` is missing");
        return -1;
    }
    return 0;
}

static int setup_running_user(struct config_t *conf, const char *login)
{
    static struct passwd passwdbuf;
    static h2o_iovec_t buf;

    if (buf.base == NULL) {
        long l = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (l == -1) {
            perror("failed to obtain sysconf(_SC_GETPW_R_SIZE_MAX)");
            return -1;
        }
        buf.len = (size_t)l;
        buf.base = h2o_mem_alloc(buf.len);
    }

    if (getpwnam_r(login, &passwdbuf, buf.base, buf.len, &conf->running_user) != 0) {
        perror("getpwnam_r");
        return -1;
    }
    if (conf->running_user == NULL)
        return -1;

    return 0;
}

static int on_config_user(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, const char *config_file, yoml_t *config_node)
{
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->globalconf);

    if (setup_running_user(conf, config_node->data.scalar) != 0) {
        h2o_configurator_errprintf(cmd, config_file, config_node, "user:%s does not exist", config_node->data.scalar);
        return -1;
    }

    return 0;
}

static int on_config_max_connections(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, const char *config_file, yoml_t *config_node)
{
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->globalconf);
    return h2o_configurator_scanf(cmd, config_file, config_node, "%d", &conf->max_connections);
}


static int on_config_num_threads(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, const char *config_file, yoml_t *config_node)
{
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->globalconf);
    return h2o_configurator_scanf(cmd, config_file, config_node, "%u", &conf->num_threads);
}

static void usage_print_directives(h2o_globalconf_t *conf)
{
    h2o_linklist_t *node;
    size_t i;

    for (node = conf->configurators.next; node != &conf->configurators; node = node->next) {
        h2o_configurator_t *configurator = H2O_STRUCT_FROM_MEMBER(h2o_configurator_t, _link, node);
        for (i = 0; i != configurator->commands.size; ++i) {
            h2o_configurator_command_t *cmd = configurator->commands.entries + i;
            const char **desc;
            printf("  %s: [%s%s%s]\n", cmd->name,
                ("g") + ((cmd->flags & H2O_CONFIGURATOR_FLAG_GLOBAL) == 0),
                ("h") + ((cmd->flags & H2O_CONFIGURATOR_FLAG_HOST) == 0),
                ("p") + ((cmd->flags & H2O_CONFIGURATOR_FLAG_PATH) == 0));
            for (desc = cmd->description; *desc != NULL; ++desc)
                printf("    %s\n", *desc);
        }
        printf("\n");
    }
}

yoml_t *load_config(const char *fn)
{
    FILE *fp;
    yaml_parser_t parser;
    yoml_t *yoml;

    if ((fp = fopen(fn, "rb")) == NULL) {
        fprintf(stderr, "could not open configuration file:%s:%s\n", fn, strerror(errno));
        return NULL;
    }
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);

    yoml = yoml_parse_document(&parser, NULL);

    if (yoml == NULL)
        fprintf(stderr, "failed to parse configuration file:%s:line %d:%s\n", fn, (int)parser.problem_mark.line, parser.problem);

    yaml_parser_delete(&parser);

    return yoml;
}

static void setup_signal_handlers(void)
{
    /* ignore SIGPIPE */
    h2o_set_signal_handler(SIGPIPE, SIG_IGN);
    /* use SIGCONT for notifying the worker threads */
    h2o_thread_initialize_signal_for_notification(SIGCONT);
}

static int num_connections(struct config_t *conf, int delta)
{
    return __sync_fetch_and_add(&conf->state._num_connections, delta);
}

static void on_socketclose(void *data)
{
    h2o_context_t *ctx = data;
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->globalconf);
    int prev_num_connections = num_connections(conf, -1);

    if (prev_num_connections == conf->max_connections) {
        /* ready to accept new connections. wake up all the threads! */
        if (conf->thread_ids != NULL) {
            unsigned i;
            for (i = 0; i != conf->num_threads; ++i)
                h2o_thread_notify(conf->thread_ids[i]);
        } else {
            h2o_thread_notify(pthread_self());
        }
    }
}

static void on_accept(h2o_socket_t *listener, int status)
{
    struct listener_ctx_t *ctx = listener->data;
    struct config_t *conf = H2O_STRUCT_FROM_MEMBER(struct config_t, globalconf, ctx->ctx->globalconf);
    int num_accepts = 16;

    if (status == -1) {
        return;
    }

    do {
        h2o_socket_t *sock;
        if (num_connections(conf, 0) >= conf->max_connections) {
            /* active threads notifies only itself so that it could update the state at the beginning of next loop */
            h2o_thread_notify(pthread_self());
            break;
        }
        if ((sock = h2o_evloop_socket_accept(listener)) == NULL) {
            break;
        }
        num_connections(conf, 1);

        sock->on_close.cb = on_socketclose;
        sock->on_close.data = ctx->ctx;

        if (ctx->ssl_ctx != NULL)
            h2o_accept_ssl(ctx->ctx, sock, ctx->ssl_ctx);
        else
            h2o_http1_accept(ctx->ctx, sock);

    } while (--num_accepts != 0);
}

static void update_listener_state(struct config_t *conf, struct listener_ctx_t *listeners)
{
    size_t i;

    if (num_connections(conf, 0) < conf->max_connections) {
        for (i = 0; i != conf->num_listeners; ++i) {
            if (! h2o_socket_is_reading(listeners[i].sock))
                h2o_socket_read_start(listeners[i].sock, on_accept);
        }
    } else {
        for (i = 0; i != conf->num_listeners; ++i) {
            if (h2o_socket_is_reading(listeners[i].sock))
                h2o_socket_read_stop(listeners[i].sock);
        }
    }
}

static void *run_loop(void *_conf)
{
    struct config_t *conf = _conf;
    h2o_evloop_t *loop;
    h2o_context_t ctx;
    struct listener_ctx_t *listeners = alloca(sizeof(*listeners) * conf->num_listeners);
    size_t i;

    /* setup loop and context */
    loop = h2o_evloop_create();
    h2o_context_init(&ctx, loop, &conf->globalconf);

    /* setup listeners */
    for (i = 0; i != conf->num_listeners; ++i) {
        struct listener_config_t *listener_config = conf->listeners[i];
        listeners[i].ctx = &ctx;
        listeners[i].ssl_ctx = listener_config->ssl.size != 0 ? listener_config->ssl.entries[0].ctx : NULL;
        listeners[i].sock = h2o_evloop_socket_create(
            ctx.loop, listener_config->fd,
            (struct sockaddr*)&listener_config->addr, listener_config->addrlen,
            H2O_SOCKET_FLAG_IS_ACCEPT);
        listeners[i].sock->data = listeners + i;
    }
    /* and start listening */
    update_listener_state(conf, listeners);

    /* the main loop */
    while (1) {
        if (h2o_thread_is_notified())
            update_listener_state(conf, listeners);
        /* run the loop once */
        h2o_evloop_run(loop);
    }

    return NULL;
}

static void setup_configurators(struct config_t *conf)
{
    h2o_config_init(&conf->globalconf);

    {
        struct listener_configurator_t *c = (void*)h2o_configurator_create(&conf->globalconf, sizeof(*c));
        c->super.enter = on_config_listen_enter;
        c->super.exit = on_config_listen_exit;
        h2o_configurator_define_command(
            &c->super, "listen",
            H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_HOST,
            on_config_listen,
            "port at which the server should listen for incoming requests (mandatory)",
            " - if the value is a scalar, it is treated as the port number (or as the",
            "   service name)",
            " - if the value is a mapping, following properties are recognized:",
            "     port: incoming port number or service name (mandatory)",
            "     host: incoming address (default: any address)",
            "     ssl: mapping of SSL configuration using the keys below (default: none)",
            "       certificate-file: path of the SSL certificate file (mandatory)",
            "       key-file:         path of the SSL private key file (mandatory)",
            "       minimum-version:  minimum protocol version, should be one of: SSLv2,",
            "                         SSLv3, TLSv1, TLSv1.1, TLSv1.2 (default: TLSv1)",
            "       cipher-suite:     list of cipher suites to be passed to OpenSSL via",
            "                         SSL_CTX_set_cipher_list (optional)",
            " - if the value is a sequence, each element should be either a scalar or a",
            "   mapping that conform to the requirements above");
    }

    {
        h2o_configurator_t *c = h2o_configurator_create(&conf->globalconf, sizeof(*c));
        h2o_configurator_define_command(
            c, "user",
            H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
            on_config_user,
            "user under with the server should handle incoming requests (default: none)");
        h2o_configurator_define_command(
            c, "max-connections", H2O_CONFIGURATOR_FLAG_GLOBAL,
            on_config_max_connections,
            "max connections (default: 1024)");
        h2o_configurator_define_command(
            c, "num-threads", H2O_CONFIGURATOR_FLAG_GLOBAL,
            on_config_num_threads,
            "number of worker threads (default: 1)");
    }

    h2o_access_log_register_configurator(&conf->globalconf);
    h2o_file_register_configurator(&conf->globalconf);
    h2o_proxy_register_configurator(&conf->globalconf);
}

int main(int argc, char **argv)
{
    static struct config_t config = {
        {}, /* globalconf */
        0, /* dry-run */
        {}, /* server_starter */
        NULL, /* listeners */
        0, /* num_listeners */
        NULL, /* running_user */
        1024, /* max_connections */
        1, /* num_threads */
        NULL, /* thread_ids */
        {}, /* state */
    };

    const char *opt_config_file = "h2o.conf";

    setup_configurators(&config);

    { /* parse options */
        int ch;
        static struct option longopts[] = {
            { "conf", required_argument, NULL, 'c' },
            { "test", no_argument, NULL, 't' },
            { "version", no_argument, NULL, 'v' },
            { "help", no_argument, NULL, 'h' },
            { NULL, 0, NULL, 0 }
        };
        while ((ch = getopt_long(argc, argv, "c:tvh", longopts, NULL)) != -1) {
            switch (ch) {
            case 'c':
                opt_config_file = optarg;
                break;
            case 't':
                config.dry_run = 1;
                break;
            case 'v':
                printf("h2o version " H2O_VERSION "\n");
                exit(0);
            case 'h':
                printf(
                    "h2o version " H2O_VERSION "\n"
                    "\n"
                    "Usage:\n"
                    "  h2o [options]\n"
                    "\n"
                    "Options:\n"
                    "  -c, --conf FILE  configuration file (default: h2o.conf)\n"
                    "  -t, --test       tests the configuration\n"
                    "  -v, --version    prints the version number\n"
                    "  -h, --help       print this help\n"
                    "\n"
                    "Configuration File:\n"
                    "  The configuration file should be written in YAML format.  Below is the list\n"
                    "  of configuration directives; the flags indicate at which level the directives\n"
                    "  can be used; g=global, h=host, p=path.\n"
                    "\n");
                usage_print_directives(&config.globalconf);
                exit(0);
                break;
            default:
                assert(0);
                break;
            }
        }
        argc -= optind;
        argv += optind;
    }

    /* setup config.server_starter */
    if ((config.server_starter.num_fds = h2o_server_starter_get_fds(&config.server_starter.fds)) == -1)
        exit(EX_CONFIG);
    if (config.server_starter.fds != 0)
        config.server_starter.bound_fd_map = alloca(config.server_starter.num_fds);

    { /* configure */
        yoml_t *yoml;
        if ((yoml = load_config(opt_config_file)) == NULL)
            exit(EX_CONFIG);
        if (h2o_configurator_apply(&config.globalconf, opt_config_file, yoml) != 0)
            exit(EX_CONFIG);
        yoml_free(yoml);
    }

    /* check if all the fds passed in by server::starter were bound */
    if (config.server_starter.fds != NULL) {
        size_t i;
        int all_were_bound = 1;
        for (i = 0; i != config.server_starter.num_fds; ++i) {
            if (! config.server_starter.bound_fd_map[i]) {
                fprintf(stderr, "no configuration found for fd:%d passed in by $SERVER_STARTER_PORT\n", config.server_starter.fds[i]);
                all_were_bound = 0;
            }
        }
        if (! all_were_bound) {
            fprintf(stderr, "note: $SERVER_STARTER_PORT was \"%s\"\n", getenv("SERVER_STARTER_PORT"));
            return EX_CONFIG;
        }
    }

    unsetenv("SERVER_STARTER_PORT");

    if (config.dry_run) {
        printf("configuration OK\n");
        return 0;
    }

    if (config.running_user != NULL) {
        if (h2o_setuidgid(config.running_user) != 0) {
            fprintf(stderr, "failed to change the running user (are you sure you are running as root?)\n");
            return EX_OSERR;
        }
    } else {
        if (getuid() == 0) {
            if (setup_running_user(&config, "nobody") == 0) {
                fprintf(stderr, "cowardly switching to nobody; please use the `user` directive to set the running user\n");
            } else {
                fprintf(stderr, "refusing to run as root (and failed to switch to `nobody`); you can use the `user` directive to set the running user\n");
                return EX_CONFIG;
            }
        }
    }

    setup_signal_handlers();

    fprintf(stderr, "h2o server (pid:%d) is ready to serve requests\n", (int)getpid());

    if (config.num_threads <= 1) {
        run_loop(&config);
    } else {
        config.thread_ids = alloca(sizeof(pthread_t) * config.num_threads);
        unsigned i;
        for (i = 0; i != config.num_threads; ++i) {
            pthread_create(config.thread_ids + i, NULL, run_loop, &config);
        }
        for (i = 0; i < config.num_threads; ++i) {
            pthread_join(config.thread_ids[i], NULL);
        }
    }

    return 0;
}
