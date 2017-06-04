/*
 * ssl_utils.h
 *
 *  Created on: 08-Jan-2017
 *      Author: abhinav
 */

#ifndef SSL_UTILS_H_
#define SSL_UTILS_H_


#include "openssl/ssl.h"
#include "openssl/err.h"
#include "openssl/opensslconf.h"



void close_ssl(SSL *ssl , int fd);
SSL_CTX *init_openssl_server();
SSL_CTX *init_openssl_client();
void cleanup_openssl(SSL_CTX * ctx);

ssize_t write_ssl_auto_handshake(SSL *ssl , char *data, size_t len, bool is_ssl);
ssize_t read_ssl( SSL*ssl, void *buf, size_t nbyte , bool is_ssl);
#endif /* SSL_UTILS_H_ */
