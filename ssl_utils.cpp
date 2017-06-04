/*
 * ssl_utils.cpp
 *
 *  Created on: 08-Jan-2017
 *      Author: abhinav
 */



#include "ssl_utils.h"
#include <unistd.h>
#include <memory>
#include "spdlog/spdlog.h"
#include "libco/co_routine.h"

/*Cipher list to be used*/
#define CIPHER_LIST "AES128-SHA"
extern std::shared_ptr<spdlog::logger> server_logger;


void close_ssl(SSL *ssl , int fd){
	close(fd);
	SSL_shutdown(ssl);
	SSL_free(ssl);
	server_logger->info("Closing ssl: {}", fd);
}



ssize_t write_ssl_auto_handshake(SSL *ssl , char *data, size_t len, bool is_ssl){

	ssize_t bytes_sent =  0;

	while(bytes_sent < len){

		ssize_t _tmp_sent =0;
		if(is_ssl){
			ERR_clear_error();
			while((_tmp_sent = SSL_write(ssl, data+bytes_sent, len-bytes_sent))<0){
				int ssl_err = SSL_get_error(ssl,_tmp_sent);
				if((ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ)){
					//connection negotiation issues, connection in pending state.
					continue;
				}
				break;
			}
		}
		else{
			_tmp_sent =  write(SSL_get_fd(ssl), data+bytes_sent, len-bytes_sent);
		}
		if(_tmp_sent<0){
			return _tmp_sent;
		}
		bytes_sent +=_tmp_sent;
	}
	return bytes_sent;
}

ssize_t read_ssl( SSL*ssl, void *buf, size_t nbyte, bool is_ssl ){
	ssize_t bytes_read = 0;
	if(is_ssl){
		while(true){
			ERR_clear_error();
			bytes_read =  SSL_read( ssl, buf, nbyte );
			if(bytes_read< 0 && errno!=LIBCO_POLL_TIMEOUT){
				int ssl_err = SSL_get_error(ssl,bytes_read);
				if((ssl_err == SSL_ERROR_WANT_WRITE)){
					//connection negotiation issues, connection in pending state.
					continue;
				}
			}
			break;
		}
		return bytes_read;
	}
	else{
		return read( SSL_get_fd(ssl),  buf, nbyte );
	}
}


SSL_CTX *init_openssl_client(){
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = SSLv23_client_method();//will do all ssl and tls negotiations despite its name.

    ctx = SSL_CTX_new(method);
    if (!ctx) {
		perror("Unable to create SSL context");
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
    }

	if (SSL_CTX_use_certificate_file(ctx, "allspark_cert.pem", SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}

	SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

static int ssl_session_ctx_id = 1;
SSL_CTX *init_openssl_server(){

	SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    ERR_load_BIO_strings();
	ERR_load_crypto_strings();

    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = SSLv23_server_method();//will do all ssl and tls negotiations despite its name.



    ctx = SSL_CTX_new(method);
    if (!ctx) {
		perror("Unable to create SSL context");
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
    }

    /*Set the Cipher List*/
    if (SSL_CTX_set_cipher_list(ctx, CIPHER_LIST) <= 0) {
      printf("Error setting the cipher list.\n");
      exit(0);
    }
	/* Set the key and cert */
	if (SSL_CTX_use_certificate_file(ctx, "allspark_cert.pem", SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}

    /*Load the password for the Private Key*/
    //SSL_CTX_set_default_passwd_cb_userdata(ctx,KEY_PASSWD);

	if (SSL_CTX_use_PrivateKey_file(ctx, "allspark_key.pem", SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}


//TODO: https://www.ibm.com/support/knowledgecenter/SSB23S_1.1.0.12/gtps7/s5sple1.html
// add cipher list, and trusted authorities
//	if (!SSL_CTX_load_verify_locations(ctx, RSA_SERVER_CA_CERT, NULL)) {
//	   ERR_print_errors_fp(stderr);
//	   exit(1);
//	}
//  /* Set CA list used for client authentication. */
//	  if (SSL_ctx_load_and_set_client_CA_file(ctx,CA_FILE) <1) {
//	    exit(0);
//	  }


	/* Set to require peer (client) certificate verification */
//	SSL_CTX_set_verify_depth(ctx,1);
//	SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,NULL);

	SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE |
	                       SSL_OP_SINGLE_ECDH_USE |
	                       SSL_OP_NO_SSLv2);

	SSL_CTX_set_session_id_context(ctx, (unsigned const char *)&ssl_session_ctx_id, sizeof(ssl_session_ctx_id));

	SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

void cleanup_openssl(SSL_CTX * ctx){
	SSL_CTX_free(ctx);
    EVP_cleanup();
}


//
//pthread_mutex_t ssl_mutex_arr[CRYPTO_num_locks()];
//
//
////static thread allocation , so we use no more than CRYPTO_num_locks
//static unsigned long openssl_get_thread_id(){
//
//}
//static void openssl_thread_locking(int mode, int id, const char *file, int line){
//
//}
