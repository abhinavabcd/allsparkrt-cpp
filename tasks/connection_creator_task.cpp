/*
 * connection_creator_task.cpp
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */

#include "connection_creator_task.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>


#include "boost/bind/placeholders.hpp"
#include "spdlog/spdlog.h"


#include "writer_task.h"
#include "../auth_utils.h"
#include "../mysql_db.h"
#include "../mem_utils.h"
#include "../utilities/conversion_utils/conversion_utils.h"
#include "../constants.h"
extern std::shared_ptr<spdlog::logger> server_logger;

extern SSL_CTX *client_ssl_ctx;
extern MemRecycler mem_recycler;
extern DatabaseWrapper db;


static void *create_and_track_new_connection_task(void *arg){
	co_enable_hook_sys();
	NewConnectionCreatorTask *task = arg;
	while(true){
		shared_ptr<Node> &node = task->node;
		shared_ptr<WrappedMessage> &msg = task->message;
		ThreadHandle * thread_handle  = task->thread_handle;

		if(!node){
			thread_handle->connection_creator_tasks_pool.push(task);
			co_yield_ct();
			continue;
		}

		server_logger->info("New connection requested");

		int ret;
		unique_ptr<SSL , boost::function<void(SSL *)> > fd_auto_close = nullptr;
		char auth_key[150];
		int auth_key_len = 0;
		struct addrinfo hints, *result, *p;


		char encoded_auth_key[200];
		char header_buf[500];
		unique_ptr<char , boost::function<void(void *)> > header_buff = nullptr;
		struct phr_header headers[100];
		unique_ptr<Connection , boost::function<void(Connection*)> > conn = nullptr;
		int fd = -1;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if ((ret = getaddrinfo(node->addr.c_str(), node->port.c_str(), &hints, &result)) != 0){
		  server_logger->error("getaddrinfo failed for node {}", node->node_id);
		}

		for(p = result; p != NULL; p = p->ai_next){
		        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		        if (fd <0) { continue; }
		        if (connect(fd, p->ai_addr, p->ai_addrlen)==0) {
		                break;
		        }
				close(fd);
				fd = -1;
		}
		freeaddrinfo(result);

		if(fd==-1){
			server_logger->error("failed to create connection to {}", node->node_id);
			goto RESET_TASK;
		}
		/*
		 * create auth_key from node id
		 */


		try{

			SSL* ssl = SSL_new(client_ssl_ctx);						/* create new SSL connection state */
			if(!ssl || !SSL_set_fd(ssl, fd))
				goto ON_CONNECTION_CLOSED;

			fd_auto_close = unique_ptr<SSL , boost::function<void(SSL *)> >( ssl , boost::bind(&close_ssl, boost::placeholders::_1, fd ));//auto close on loop exit

			if(node->is_ssl){
				bool ssl_connect_ret = 0;
				while((ssl_connect_ret = SSL_connect(ssl))<=0){
					if(ssl_connect_ret<0 && errno!=LIBCO_POLL_TIMEOUT){
						int ssl_err = SSL_get_error(ssl,ssl_connect_ret);
						if(ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ){
							// in the process
							server_logger->error("SSL_ACCEPT_ERROR {}", ssl_connect_ret);
							continue;
						}
					}
					goto ON_CONNECTION_CLOSED;
				}
			}
			generate_auth_key( current_node->node_id, auth_key, auth_key_len);
			size_t encoded_auth_key_len = urlencode(auth_key, auth_key_len,  encoded_auth_key);

			//websocket header_buf
			ssize_t s=0;
			memcpy(header_buf, "GET /connectV3?auth_key=", 24);s+=24;
			memcpy(header_buf+s,  encoded_auth_key, encoded_auth_key_len);s+=encoded_auth_key_len;
			memcpy(header_buf+s, " HTTP/1.1\r\n",11);s+=11;
			memcpy(header_buf+s, "Upgrade: websocket\r\n", 20);s+=20;
			memcpy(header_buf+s, "Connection: Upgrade\r\n", 21);s+=21;
			memcpy(header_buf+s, "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n", 45);s+=45;
			memcpy(header_buf+s, "Sec-WebSocket-Version: 13\r\n", 27);s+=27;
			memcpy(header_buf+s, "\r\n", 2);s+=2;

			ssize_t headers_written = write_ssl_auto_handshake(ssl , header_buf, s, node->is_ssl);
			if(headers_written==s){
				//read the data from socket
				header_buff = unique_ptr<char , boost::function<void(void *)> >((char*)mem_recycler.get(2048) , boost::bind(&MemRecycler::recyle, &mem_recycler, boost::placeholders::_1, 2048));
				int pret;
				size_t buflen = 0, prevbuflen = 0, num_headers;

				int minor_version;
				int status;
				const char *msg_buf;
				size_t msg_buf_len;

				while (true) {
						/* read the request */
						ssize_t bytes_read = read_ssl( ssl, header_buff.get() + buflen, 2048-buflen , node->is_ssl);
						if (bytes_read <= 0){
							// even if timeout , we break , because it should respond in timeout
							break;
						}

						prevbuflen = buflen;
						buflen+=bytes_read;
						/* parse the request */
						num_headers = sizeof(headers) / sizeof(headers[0]);
						pret = phr_parse_response(header_buff.get(), buflen, &minor_version , &status, &msg_buf, &msg_buf_len,
												headers, &num_headers, prevbuflen);
						if (pret > 0)
							break; /* successfully parsed the request */
						else if (pret == -1){
							break;
						}
						if (buflen >= 2048){
							break;
						}
						//pret == -2 continue looping.
				}
				if(pret>0){
					//successfully established connection
					Connection * conn_temp = track_new_connection(ssl, node, thread_handle, node->is_ssl);
					if(!conn_temp){
						server_logger->error("could not track connection");
						goto ON_CONNECTION_CLOSED;
					}
					conn = unique_ptr<Connection , boost::function<void(Connection*)> >( conn_temp , boost::bind(&untrack_connection , boost::placeholders::_1, true, thread_handle));

					if(msg){
						update_intermediate_node(msg->dest_id(), conn->to_node_id);

						conn->pending_queue.push(std::move(msg));//moves it
						start_writer_task(conn.get(), thread_handle); //this will create a coroutine

						//created for the sake of connection , update in intermedaite cache
						task->message = nullptr;//destroy the shared_ptr to msg

					}


					handle_connection(conn.get(), thread_handle); // this should untrack the connection if closed/error , but you need to manually close it

				}
			}
			// connection created successfully





			//reset task
			ON_CONNECTION_CLOSED:
			if(msg!=nullptr){
				server_logger->error("new_connection_msg_send_error, dest: {}, via: {}",msg->dest_id(), task->node->node_id);
				on_message(msg, NULL, USE_DIRECT_CONNECTIONS_ONLY);//could not create a new connection , better to see if we can send or keep it in inbox
			}
			if(conn){
				server_logger->info("closing connction {} ", conn->to_node_id);
			}
		}
		catch(sql::SQLException &ex){
			server_logger->error("mysql error {} {} line {}" ,ex.getErrorCode() , ex.what() ,__LINE__);
		}
		catch(std::invalid_argument &ex){
			server_logger->error("json parse error {} line {}" ,ex.what() , __LINE__);
		}


		RESET_TASK:
			task->message = nullptr;
			task->node = nullptr;//destroy the shared_ptr

		//if conn , new connection is created

	}
}

void start_connection_creator_task(shared_ptr<Node> &node, shared_ptr<WrappedMessage> &msg, ThreadHandle *thread_handle){
	NewConnectionCreatorTask *ret;
	if(thread_handle->connection_creator_tasks_pool.empty()){
		ret = new NewConnectionCreatorTask();

		stCoRoutineAttr_t attr;
		attr.stack_size = 0;
		attr.share_stack = thread_handle->co_share_stack;
		server_logger->info("creating connection creator task");

		co_create(&ret->co_routine , &attr, create_and_track_new_connection_task , ret);
	}

	ret->node = node;
	ret->message = std::move(msg);
	ret->thread_handle = thread_handle;

	co_resume(ret->co_routine);
	return;
}


