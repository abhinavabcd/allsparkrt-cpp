/*
 * http_and_websocket_utils.cpp
 *
 *  Created on: Dec 19, 2016
 *      Author: abhinav
 */





/*
 * websocket server helpers
 */


#include "http_and_websocket_utils.h"
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>

#include "libco/co_routine.h"

#include "spdlog/spdlog.h"
#include "utilities/json/json.hpp"


#include "boost/move/unique_ptr.hpp"
#include "boost/function.hpp"
#include "boost/bind/placeholders.hpp"
#include "boost/exception/diagnostic_information.hpp"


#include "picohttpparser/picohttpparser.h"

#include "utilities/conversion_utils/conversion_utils.h"
#include "utilities/HMAC_SHA1/HMAC_SHA1.h"
#include "ssl_utils.h"
#include "mem_utils.h"
#include "config.h"



using std::string;
using std::map;
using boost::movelib::unique_ptr;
using boost::function;
// for convenience
using json = nlohmann::json;


const char * _web_socket_magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const int _web_socket_magic_size = strlen(_web_socket_magic);

extern std::shared_ptr<spdlog::logger> server_logger;
extern MemRecycler mem_recycler;
extern SSL_CTX * client_ssl_ctx;



const char *CRLF = "\r\n";
const int CRLF_SIZE = 2;


bool answer_web_socket_handshake(SSL*ssl, struct phr_header *websocket_key_header , struct phr_header *websocket_protocol_header, bool is_ssl) {
    unsigned char digest[20]; // 160 bit sha1 digest

	CSHA1 sha;
	string accept_key(websocket_key_header->value,  websocket_key_header->value_len);
	accept_key.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); //RFC6544_MAGIC_KEY

	sha.Update((unsigned char*)accept_key.c_str(), (unsigned int)accept_key.size());
	sha.Final();
	sha.GetHash(digest);

//	little endian to big endian
//	for(int i=0; i<20; i+=4) {
//		unsigned char c;
//
//		c = digest[i];
//		digest[i] = digest[i+3];
//		digest[i+3] = c;
//
//		c = digest[i+1];
//		digest[i+1] = digest[i+2];
//		digest[i+2] = c;
//	}
	//printf("DIGEST:"); for(int i=0; i<20; i++) printf("%02x ",digest[i]); printf("\n");
	char encoded_digest[50];
	int encoded_digest_length = base64_encode((const char *)digest, 20, encoded_digest); //160bit = 20 bytes/chars

	char data[500];
	size_t s = 0;
	memcpy(data, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\n", 75);s+=75;
	memcpy(data+s, "Sec-WebSocket-Accept: " , 22);s+=22;
	memcpy(data+s, encoded_digest, encoded_digest_length);s+=encoded_digest_length;
	memcpy(data+s, CRLF , CRLF_SIZE);s+=CRLF_SIZE;
	memcpy(data+s, CRLF , CRLF_SIZE);s+=CRLF_SIZE;

	bool ret = write_ssl_auto_handshake(ssl , data , s, is_ssl)>0;//complete
	return ret;
}


int make_websocket_frame(WebSocketFrameType frame_type, char* msg, size_t msg_length, char* buffer, size_t &buffer_size, bool copy_only_head=false){
	int pos = 0;
	buffer[pos++] = (unsigned char)frame_type; // text frame

	if(msg_length <= 125) {
		buffer[pos++] = msg_length;
	}
	else if(msg_length <= 65535) {
		buffer[pos++] = 126; //16 bit length follows

		buffer[pos++] = (msg_length >> 8) & 0xFF; // leftmost first
		buffer[pos++] = msg_length & 0xFF;
	}
	else { // >2^16-1 (65535)
		buffer[pos++] = 127; //64 bit length follows

		// write 8 bytes length (significant first)

		// since msg_length is int it can be no longer than 4 bytes = 2^32-1
		// padd zeroes for the first 4 bytes
		for(int i=3; i>=0; i--) {
			buffer[pos++] = 0;
		}
		// write the actual 32bit msg_length in the next 4 bytes
		for(int i=3; i>=0; i--) {
			buffer[pos++] = ((msg_length >> 8*i) & 0xFF);
		}
	}
	if(copy_only_head){
		return buffer_size = pos;
	}
	memcpy((void*)(buffer+pos), msg, msg_length);
	return (buffer_size = pos+msg_length);
}

WebSocketFrameType get_websocket_frame( char* bytes, int in_length,  char* &out_buffer, int &out_length, int *frame_header_length){
	//printf("getTextFrame()\n");
	if(in_length < 3) return INCOMPLETE_FRAME;

	unsigned char msg_opcode = (bytes[0] & 0x0f);
	unsigned char msg_fin =  ((bytes[0] & 0x80) >> 7);
	unsigned char msg_masked = ((bytes[1] & 0x80) >> 7);

	// *** message decoding

	ssize_t payload_length = 0;
	int pos = 2;
	unsigned char length_field = bytes[1] & 0x7f;
	unsigned int mask = 0;

	//printf("IN:"); for(int i=0; i<20; i++) printf("%02x ",buffer[i]); printf("\n");

	if(length_field < 126) {
		payload_length = length_field;
	}
	else if(length_field == 126) {
		payload_length |=(bytes[2] & 0xff);
		payload_length <<= 8;
		payload_length |=(bytes[3] & 0xff);
		pos += 2;
	}
	else if(length_field == 127) { //msglen is 64bit!
		memcpy(&payload_length, &bytes[pos], 8);
		pos += 8;
	}
	//printf("PAYLOAD_LEN: %08x\n", payload_length);
	if(in_length < payload_length+pos) {
		return INCOMPLETE_FRAME;
	}

	if(msg_masked) {
		mask = *((unsigned int*)(bytes+pos));
		//printf("MASK: %08x\n", mask);
		pos += 4;

		// unmask data:
		unsigned char* c = bytes+pos;
		for(int i=0; i<payload_length; i++) {
			c[i] = c[i] ^ ((unsigned char*)(&mask))[i%4];
		}
	}

	out_buffer = (bytes+pos);
	out_length = payload_length;

	if(frame_header_length!=NULL){
		*frame_header_length = pos;
	}
	//printf("TEXT: %s\n", out_buffer);

	if(msg_opcode == 0x0) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME; // continuation frame ?
	if(msg_opcode == 0x1) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME;
	if(msg_opcode == 0x2) return (msg_fin)?BINARY_FRAME:INCOMPLETE_BINARY_FRAME;
	if(msg_opcode == 0x8) return CLOSING_FRAME;
	if(msg_opcode == 0x9) return PING_FRAME;
	if(msg_opcode == 0xA) return PONG_FRAME;

	return ERROR_FRAME;
}



void parse_path(const char *path , int path_len , int &folder_path_length, map<StringPtr, StringPtr, StringPtrCompare>& query_params){
	size_t query_start = path_len;
	for(int i=0;i<path_len;i++){
		if(path[i]=='?'){
			folder_path_length = i;
			query_start = i+1;
			break;
		}
	}
	ssize_t last_equalto_token = -1;
	size_t prev_start = query_start;

	for(int i=query_start;i<path_len;i++){
		if((path[i]=='&' || i==path_len-1) && last_equalto_token!=-1){
			StringPtr s;//created on stack
			s.ptr = (char *)(path+last_equalto_token+1);
			s.len = i-last_equalto_token- (i==path_len-1 ? 0 : 1);
			if(last_equalto_token-prev_start > 0)
				query_params[StringPtr(path+prev_start, last_equalto_token-prev_start)] = s;//copies the structure into the maps internal tree
			last_equalto_token = -1;
			prev_start = i+1;
			continue;
		}
		else if(path[i]=='=' && last_equalto_token ==-1){
			last_equalto_token = i;
		}
	}
}


void print_addr_info(addrinfo *res){
     	char addrstr[100];
		void *ptr;
		inet_ntop(res->ai_family, res->ai_addr->sa_data, addrstr, 100);

		switch (res->ai_family) {
		case AF_INET:
			ptr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
			break;
		case AF_INET6:
			ptr = &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
			break;
		}
		inet_ntop(res->ai_family, ptr, addrstr, 100);


		in_port_t port = 0;
	    if (res->ai_addr->sa_family == AF_INET) {
	        port = (((struct sockaddr_in*)res->ai_addr)->sin_port);
	    }
	    else{
	    	port = (((struct sockaddr_in6*)res->ai_addr)->sin6_port);
	    }
		printf("IPv%d address: %s (%s) port %hu \n",
				res->ai_family == PF_INET6 ? 6 : 4, addrstr,
				res->ai_canonname , ntohs(port));
}

bool send_a_push_notification(const string &gcm_key, const string *node_id, const string &payload, int type){

	if(gcm_key.empty()) return false;
	if(node_id!=NULL){
		server_logger->info("sending push notification to {}", *node_id);
	}
	struct addrinfo hints, *result, *p;
	int ret;
	int fd = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	if ((ret = getaddrinfo("fcm.googleapis.com", "443", &hints, &result)) != 0){
	  server_logger->error("getaddrinfo failed for fcm {}", "fcm");
	  return false;
	}

//	https://fcm.googleapis.com/fcm/send
//	Content-Type:application/json
//	Authorization:key=AIzaSyZ-1u...0GBYzPu7Udno5aA


	for(p = result; p != NULL; p = p->ai_next){
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd <0) { continue; }
		if (connect(fd, p->ai_addr, p->ai_addrlen)==0) {
			//print_addr_info(p);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(result);
	if(fd==-1){
		server_logger->error("failed to create connection to fcm");
		return false;
	}
	/*
	 * create auth_key from node id
	 */



	SSL* ssl = SSL_new(client_ssl_ctx);						/* create new SSL connection state */
	SSL_set_fd(ssl, fd);
	unique_ptr<SSL , boost::function<void(SSL *  )> > fd_auto_close( ssl , boost::bind(&close_ssl, boost::placeholders::_1, fd ));//auto close on loop exit

	if(SSL_connect(ssl)<0){
		return false;
	}
	X509 *cert = SSL_get_peer_certificate(ssl);
	if(!cert){
		return false;
	}
	X509_free(cert);//i don't know why i am doing this
	//websocket headers
	char headers[1024];
	size_t s=0;
	memcpy(headers, "POST /fcm/send HTTP/1.1\r\n",25);s+=25;
	memcpy(headers+s, "Content-Type: application/json\r\n", 32);s+=32;
	memcpy(headers+s, "Connection: close\r\n", 19); s+=19;
	memcpy(headers+s, "Host: fcm.googleapis.com\r\n", 26); s+=26;
	memcpy(headers+s, "Authorization: key=", 19);s+=19;
	memcpy(headers+s, &(server_config.gcm_authorization_key)[0], server_config.gcm_authorization_key.length());s+=server_config.gcm_authorization_key.length();
	memcpy(headers+s, "\r\n", 2);s+=2;


	json data = {
			{"to", gcm_key},
			{"data", {
					{"type", type},
					{"payload",payload}
				}
			},
			{"notification", {
						{"body" , "" },
						{"title" , "You have unread notifications"}
					}
			}
	};

	string json_data = data.dump();

	char content_length[10];
	int n_digits = itoa(json_data.length(), content_length, 10);

	memcpy(headers+s, "Content-Length: ", 16);s+=16;
	memcpy(headers+s, content_length, n_digits);s+=n_digits;
	memcpy(headers+s, "\r\n\r\n", 4);s+=4;

	memcpy(headers+s, &json_data[0], json_data.length());s+=json_data.length();



	ssize_t headers_written =0;
	ERR_clear_error();
	while((headers_written = SSL_write(ssl, headers, s))<0){
		int ssl_err = SSL_get_error(ssl,headers_written);
		if((ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ)){
			//connection negotiation issues, connection in pending state.
			ERR_clear_error();
			continue;
		}
		break;
	}
	if(headers_written==s){
		//read the data from socket
		unique_ptr<char , boost::function<void(void *)> > header_buff( (char*)mem_recycler.get(2048) , boost::bind(&MemRecycler::recyle, &mem_recycler, boost::placeholders::_1, 2048));
		int pret;
		struct phr_header headers[100];
		size_t buflen = 0, prevbuflen = 0, method_len, num_headers;

		int minor_version;
		int status;
		const char *msg_buf;
		size_t msg_buf_len;

		while (true) {
				/* read the request */
				ERR_clear_error();
				ssize_t bytes_read = SSL_read( ssl, header_buff.get() + buflen, 2048-buflen );
				if (bytes_read <= 0){

					if(errno!=LIBCO_POLL_TIMEOUT){
						//ssl related error , handshaking in between
						int ssl_err = SSL_get_error(ssl,bytes_read);
						if((ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ)){
							//connection negotiation issues, connection in pending state.
							continue;
						}
					}
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
			return true;
		}
		return false;
	}
	return false;
}

//request based functions

phr_header * Request::get_single_header(const string &header_val){
	for(int i=0;i<num_headers;i++){
		if(strncasecmp(headers[i].name , &header_val[0] , std::min(header_val.length(), headers[i].name_len))==0){
			return &headers[i];
		}
	}
	return NULL;
}

StringPtr* Request::get_query_param(const string &query){
	map<StringPtr, StringPtr>::iterator it = this->query_params.find(StringPtr(&query[0] , query.length()));
	if(it == query_params.end()){
		return NULL;
	}
	return &it->second;
}


Request::~Request(){
	if(this->headers_map!=NULL){
		delete headers_map;
	}
}

void Request::reset(){
	node_id = NULL;
	headers_map = NULL;
	post_data.ptr = NULL;
	post_data.len = 0;
	num_headers = 0;
	query_params.clear();
	is_post_req = false;
	keep_alive = false;
}


phr_header * get_post_data(phr_header *headers, int num_headers){
	if(strncmp(headers[num_headers-1].name , "_POST_" , 6 )==0){
		return headers+(num_headers-1);
	}
	return NULL;
}


