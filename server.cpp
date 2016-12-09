/*
 * server.cpp
 *
 *  Created on: Nov 27, 2016
 *      Author: abhinav
 *
 *      single file to handle entire scalable server.
 *      uses libco folly tbb
 *      send a protobuff Message , we see destid and forward it to the client , it it's not present , we simpley lookit up in db and forward it to the client.
 *      if it's not at all available , we simpley drop the message into db with a sequential id , that client can query later.
 *		structured conversation blocks are simple messages but they should also be created on a separate server , all responses to the structured blocks
 *
 */

#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>


#include "tbb/concurrent_hash_map.h"
#include "tbb/concurrent_queue.h"
#include "libco/co_routine.h"
#include "server.h"

#include "utilities/HMAC_SHA1/SHA1.h"
#include "picohttpparser/picohttpparser.h"
#include <pthread.h>
#include <map>
#include <vector>

using std::map;
using std::pair;
using std::make_pair;
using folly::fbvector;
using std::vector;
using tbb::concurrent_hash_map;


/*
 * Utility functions..
 */
static int set_non_blocking(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}

class MemRecycler{
public:
	concurrent_hash_map<int , concurrent_queue<void *>*> mem_pool;

	void* get(int size_of_memory){
		void * ret = NULL;
		concurrent_hash_map<int , concurrent_queue<void *>*>::accessor mem_pool_accessor;
		if(!mem_pool.find(mem_pool_accessor, size_of_memory)){
			mem_pool.insert(mem_pool_accessor, size_of_memory);
			mem_pool_accessor->second  = new concurrent_queue<void *>();
		}
		if(!mem_pool_accessor->second->try_pop(ret)){//pool is empty , all ptrs are used up
			ret = malloc(size_of_memory);//allocate new memory
		}
		mem_pool_accessor.release();
		return ret;
	}

	/*
	 * size in bytes
	 */
	void recyle(void *ptr , int size_of_memory){
		concurrent_hash_map<int , concurrent_queue<void *>*>::accessor mem_pool_accessor;
		if(!mem_pool.find(mem_pool_accessor, size_of_memory)){
			mem_pool.insert(mem_pool_accessor, size_of_memory);
			mem_pool_accessor->second  = new concurrent_queue<void *>();
		}
		mem_pool_accessor->second->push(ptr);
		mem_pool_accessor.release();
	}
} mem_recycler;



/*
 * utility functions end
 */

/*
 * Global variables constants
 */
Node *current_node;

char PROTOBUF = 1;
char JSON = 0;


/*
 * Global variables and constants end
 */



/*
 *  http server calls
 */
void websocket_handler_v3(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void join_session(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void unjoin_session(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void reveal_anonymity(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void get_session_info(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void create_session(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}
void push_message(int sockfd, map<string, fbvector<string>>& query_params, map<string, string>& headers){

}

fbvector<pair<string, web_request_handler>> web_request_handlers;

/*
 * end HTTP server calls
 */



/*
 * websocket server helpers
 */

enum WebSocketFrameType {
	ERROR_FRAME=0xFF00,
	INCOMPLETE_FRAME=0xFE00,

	OPENING_FRAME=0x3300,
	CLOSING_FRAME=0x3400,

	INCOMPLETE_TEXT_FRAME=0x01,
	INCOMPLETE_BINARY_FRAME=0x02,

	TEXT_FRAME=0x81,
	BINARY_FRAME=0x82,

	PING_FRAME=0x19,
	PONG_FRAME=0x1A
};

const char *_web_socket_headers = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\n\0";
const int _web_socket_headers_size = strlen(_web_socket_headers);

const char * _web_socket_magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11\0";
const int _web_socket_magic_size = strlen(_web_socket_magic);

const char *_web_socket_accept_1 = "Sec-WebSocket-Accept: \0";
const int _web_socket_accept_1_size = strlen(_web_socket_accept_1);

const char *CRLF = "\r\n\0";
const int CRLF_SIZE = strlen(CRLF);

bool answer_handshake(int fd, struct phr_header *websocket_key_header , struct phr_header *websocket_protocol_header) {
    unsigned char digest[20]; // 160 bit sha1 digest

	CSHA1 sha;
	string accept_key;
	accept_key.append(websocket_key_header->value,  websocket_key_header->value_len);
	accept_key.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); //RFC6544_MAGIC_KEY

	sha.Update((unsigned char *)accept_key.c_str(), accept_key.size());
	sha.Final();
	sha.GetHash(digest);

	//little endian to big endian
	for(int i=0; i<20; i+=4) {
		unsigned char c;

		c = digest[i];
		digest[i] = digest[i+3];
		digest[i+3] = c;

		c = digest[i+1];
		digest[i+1] = digest[i+2];
		digest[i+2] = c;
	}
	//printf("DIGEST:"); for(int i=0; i<20; i++) printf("%02x ",digest[i]); printf("\n");
	accept_key = base64_encode((const unsigned char *)digest, 20); //160bit = 20 bytes/chars

	int bytes_to_send = 0;
    int ret = write( fd, _web_socket_headers, _web_socket_headers_size)+
			write(fd , _web_socket_accept_1, _web_socket_accept_1_size)+
			write(fd , accept_key.c_str() , accept_key.length())+
			write(fd , CRLF , CRLF_SIZE);

	bytes_to_send+=(_web_socket_headers_size+_web_socket_accept_1_size+accept_key.length()+CRLF_SIZE);
	if(websocket_protocol_header!=NULL ) {
		ret+= write( fd, websocket_protocol_header->value , websocket_protocol_header->value_len);
		ret+=write(fd , CRLF , CRLF_SIZE);
		bytes_to_send+=(websocket_protocol_header->value_len+CRLF_SIZE);
	}

	ret+=write(fd , CRLF , CRLF_SIZE);//complete
	bytes_to_send += CRLF_SIZE;
	return ret==bytes_to_send;
}


int make_websocket_frame(WebSocketFrameType frame_type, unsigned char* msg, int msg_length, unsigned char* buffer, int buffer_size){
	int pos = 0;
	int size = msg_length;
	buffer[pos++] = (unsigned char)frame_type; // text frame

	if(size <= 125) {
		buffer[pos++] = size;
	}
	else if(size <= 65535) {
		buffer[pos++] = 126; //16 bit length follows

		buffer[pos++] = (size >> 8) & 0xFF; // leftmost first
		buffer[pos++] = size & 0xFF;
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
			buffer[pos++] = ((size >> 8*i) & 0xFF);
		}
	}
	memcpy((void*)(buffer+pos), msg, size);
	return (size+pos);
}

WebSocketFrameType get_websocket_frame( char* in_buffer, int in_length,  char* &out_buffer, int &out_length)
{
	//printf("getTextFrame()\n");
	if(in_length < 3) return INCOMPLETE_FRAME;

	unsigned char msg_opcode = in_buffer[0] & 0x0F;
	unsigned char msg_fin = (in_buffer[0] >> 7) & 0x01;
	unsigned char msg_masked = (in_buffer[1] >> 7) & 0x01;

	// *** message decoding

	int payload_length = 0;
	int pos = 2;
	int length_field = in_buffer[1] & (~0x80);
	unsigned int mask = 0;

	//printf("IN:"); for(int i=0; i<20; i++) printf("%02x ",buffer[i]); printf("\n");

	if(length_field <= 125) {
		payload_length = length_field;
	}
	else if(length_field == 126) { //msglen is 16bit!
		payload_length = in_buffer[2] + (in_buffer[3]<<8);
		pos += 2;
	}
	else if(length_field == 127) { //msglen is 64bit!
		payload_length = in_buffer[2] + (in_buffer[3]<<8);
		pos += 8;
	}
	//printf("PAYLOAD_LEN: %08x\n", payload_length);
	if(in_length < payload_length+pos) {
		return INCOMPLETE_FRAME;
	}

	if(msg_masked) {
		mask = *((unsigned int*)(in_buffer+pos));
		//printf("MASK: %08x\n", mask);
		pos += 4;

		// unmask data:
		unsigned char* c = in_buffer+pos;
		for(int i=0; i<payload_length; i++) {
			c[i] = c[i] ^ ((unsigned char*)(&mask))[i%4];
		}
	}

	out_buffer = (in_buffer+pos);
	out_length = payload_length;

	//printf("TEXT: %s\n", out_buffer);

	if(msg_opcode == 0x0) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME; // continuation frame ?
	if(msg_opcode == 0x1) return (msg_fin)?TEXT_FRAME:INCOMPLETE_TEXT_FRAME;
	if(msg_opcode == 0x2) return (msg_fin)?BINARY_FRAME:INCOMPLETE_BINARY_FRAME;
	if(msg_opcode == 0x9) return PING_FRAME;
	if(msg_opcode == 0xA) return PONG_FRAME;

	return ERROR_FRAME;
}

struct StringPtr{
	char * ptr;
	int len;
};

int parse_path(const char *path , int path_len , int &folder_path_len, map<string, StringPtr>& query_params){
	int query_start = 0;
	for(int i=0;i<path_len;i++){
		if(path[i]=='?'){
			query_start = i+1;
			break;
		}
	}

	int prev_start = query_start;
	int last_equalto_token = 0;
	for(int i=query_start;i<path_len;i++){
		if(path[i]=='&' || i==path_len-1){
			StringPtr s;//created on stack
			s.ptr = (char *)(path+last_equalto_token+1);
			s.len = i-last_equalto_token - (path[i]=='&'?1:0);
			query_params[string(path+prev_start, last_equalto_token-prev_start)] = s;//copies the structure into the maps internal tree
			prev_start = i+1;
			continue;
		}
		else if(path[i]=='='){
			last_equalto_token = i;
		}
	}
}



/*
 * end websocket helpers
 */



/*
 * received a new message from a connection
 * if connection is null , i means an internal transfered message
 */
void on_new_message(Message *message, Connection* conn){

}

void *handle_new_connection(void *_fd){
	//sample the protocol, read the header data, read the auth key, identify connection as json/protobuff, create the connection block.
	//all data be created on stack
	co_enable_hook_sys();
	int fd = (int)_fd;
	set_non_blocking(fd);
	//make non blocking read/write
	//keep reading data until validity , if error , simply close socket any where.
	//read the http headers..
	char *header_buff = (char*)mem_recycler.get(2048);
	char *method, *path;
	int pret, minor_version;
	struct phr_header headers[100];
	size_t buflen = 0, prevbuflen = 0, method_len, path_len, num_headers;

	/*
	 * read the headers of request
	 */
	while (true) {
	    /* read the request */
		int bytes_read = read( fd,header_buff+buflen, 2048-buflen );
	    if (bytes_read <= 0){
	    	goto CLOSE;
	    }
	    prevbuflen = buflen;
	    buflen+=bytes_read;
	    /* parse the request */
	    num_headers = sizeof(headers) / sizeof(headers[0]);
	    pret = phr_parse_request(header_buff, buflen, &method, &method_len, &path, &path_len,
	                             &minor_version, headers, &num_headers, prevbuflen);
	    if (pret > 0)
	        break; /* successfully parsed the request */
	    else if (pret == -1){
	        goto CLOSE;
	    }
	    if (buflen >= 2048){
	       goto CLOSE;
	    }
	    //pret == -2 continue looping.
	}
	//if websocket request parse handshake and answer , create a connection object with the node_id and keep it in read mode reading frames from the connection
	bool is_web_socket = false;
	char message_data_type = JSON;
	struct phr_header *websocket_key_header = NULL;
	struct phr_header *websocket_protocol_header = NULL;
	for(int i=0;i<num_headers;i++){
		if(strncmp(headers[i].name , "Sec-WebSocket-Key", strlen("Sec-WebSocket-Key\0"))){
			is_web_socket = true;
			websocket_key_header = headers+i;
		}
		else if(strncmp(headers[i].name , "Sec-WebSocket-Version", strlen("Sec-WebSocket-Version\0"))){
			websocket_protocol_header = headers+i;
		}
		else if(strncmp(headers[i].name , "Message-tranfer-protocol", strlen("Message-tranfer-protocol\0"))){
			message_data_type = strncmp(headers[i].value , "protobuf", headers[i].value_len)?PROTOBUF:JSON;
		}
	}

	//parse path and query params from the request
	map<string, StringPtr> query_params;
	int query_start = 0;
	int folder_path_len = 0;

	parse_path(path , path_len,  folder_path_len , query_params);


	if(query_params.find("auth_key")==query_params.end()){
		goto CLOSE;
	}
	/*
	 * validate the connection , should be encrypted with a key
	 */
	StringPtr auth_key_ptr = query_params["auth_key"];
	char decoded_buf[auth_key_ptr.len];
	int decoded_length = base64_decode(auth_key_ptr.ptr , auth_key_ptr.len, decoded_buf);

	char *digest;
	int digest_length;
	char *auth_data;
	int auth_data_length
	if(get_websocket_frame(decoded_buf, decoded_length, digest, digest_length) != TEXT_FRAME || get_websocket_frame(digest+digest_length, decoded_length - (digest+digest_length - decoded_buf), auth_data, auth_data_length) != TEXT_FRAME){
		goto CLOSE;
	}

	CHMAC_SHA1 HMAC_SHA1;
	char digest_calculated[server_secret_key.length()];
	HMAC_SHA1.HMAC_SHA1((unsigned char *)auth_data, auth_data_length, server_secret_key.c_str(), server_secret_key.length(), (unsigned char *) digest_calculated);

	if(!strncmp(digest_calculated, digest, digest_length)){
		//invalid request
		goto CLOSE;
	}

	if(is_web_socket){
		//keep in read mode and keep reading frames
	}
	else{
		// must be http , return response directly

	}

	mem_recycler.recyle(header_buff, 2048);
	return NULL;
	CLOSE:
		mem_recycler.recyle(header_buff, 2048);
		close(fd);
		return NULL;

}





/*
 * intializes the server thread with a thread handle
 */
void *init_server_thread(void *arg){
	//TODO: multi threaded , accepts connections and delegates to the threads

	ThreadHandle *thread_handle = arg;
	stCoEpoll_t * ev = co_get_epoll_ct(); //ct = current thread

	while(!thread_handle->new_sockets.empty()){
		//start the co routine to handle new
		int fd = -1;
		thread_handle->new_sockets.try_pop(fd);
		stCoRoutine_t *co;
		co_create( &co ,NULL, handle_new_connection, (int*)fd);
		co_resume( co );
	}

	while(!thread_handle->messages_queue.empty()){
		Message *message;
		thread_handle->messages_queue.try_pop(message);
		//need no start in a co routine, as this will just wake up relevant co routines to process.
		on_new_message(message , NULL);
	}

	co_eventloop( ev,NULL,0 );//this will wake up all sockets that have something to read/write to
	return 0;
}



/*
 * Basic starting server and socket initializations
 */

static int g_listen_fd = -1;


static void set_addr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr){
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0")
		|| 0 == strcmp(pszIP,"*")
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;
}

static int create_tcp_socket(const unsigned short shPort /* = 0 */,const char *pszIP /* = "*" */,bool bReuse /* = false */){
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			set_addr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}

static inline void set_timeouts(int fd , int read_timeout_secs, int write_timeout_secs);

int main(int argc,char *argv[]){
	//1. inititialize database with auto-scalable pool
	//2. start server with handlers and port

	/*
	 * handlers = http path regex , function to call
	 */

	//TODO: start_server_threads like number of cores ,
	// now start a socket server and accept connections
	// delegate them to the threads , by putting them on the thread handlers in round robin fashion


	unsigned int nthreads = std::thread::hardware_concurrency();
	if(nthreads==0){
		nthreads = atoi( argv[3] );
	}
	pthread_t tid[ nthreads ];
	ThreadHandle thread_handles[nthreads];

	for(int i=0;i<nthreads;i++){
		ThreadHandle * handle = thread_handles+i;
		pthread_create( tid + i,NULL, init_server_thread,handle);
		handle->thread_ref = tid + i;
	}



	web_request_handlers.push_back(make_pair("^/connectV3", websocket_handler_v3));
	web_request_handlers.push_back(make_pair("^/join_session", join_session));
	web_request_handlers.push_back(make_pair("^/unjoin_session", unjoin_session));
	web_request_handlers.push_back(make_pair("^/reveal_anonymity", reveal_anonymity));
	web_request_handlers.push_back(make_pair("^/get_session_info", get_session_info));
	web_request_handlers.push_back(make_pair("^/create_session", create_session));
	web_request_handlers.push_back(make_pair("^/push_message", push_message));

	const char *ip = argv[1];
	int port = atoi( argv[2] );
//	int worker_tasks_count = atoi( argv[3] );
//	int process_countt = atoi( argv[4] );

	g_listen_fd = create_tcp_socket( port,ip,true );
	listen( g_listen_fd, 1024 );//listen with abacklog of 1024 sockets

	int thread_n_use = 0;
	while(true){
		struct sockaddr_in addr; //maybe sockaddr_un;
		memset( &addr,0,sizeof(addr) );
		socklen_t len = sizeof(addr);
		int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);

		//set the timelimits to zero
		set_timeouts(fd, 0,10);//read no timeout , write 10 seconds

		//keep distributing to threads in round robin way
		thread_handles[thread_n_use].new_sockets.push(fd);
		thread_n_use = (thread_n_use+1)%nthreads;
	}

}
