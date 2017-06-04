/*
 * http_and_websocket_utils.h
 *
 *  Created on: Dec 19, 2016
 *      Author: abhinav
 */

#ifndef HTTP_AND_WEBSOCKET_UTILS_H_
#define HTTP_AND_WEBSOCKET_UTILS_H_

#include <map>
#include "ssl_utils.h"
#include "utils.h"
#include <cstring>
#include <stdio.h>
#include "boost/shared_ptr.hpp"
#include "boost/move/unique_ptr.hpp"
#include "boost/move/make_unique.hpp"
#include "picohttpparser/picohttpparser.h"

using std::map;
using std::string;
using boost::shared_ptr;
using boost::movelib::unique_ptr;
using boost::movelib::make_unique;

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

// http://tools.ietf.org/html/rfc6455#section-5.2  Base Framing Protocol
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-------+-+-------------+-------------------------------+
// |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
// |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
// |N|V|V|V|       |S|             |   (if payload len==126/127)   |
// | |1|2|3|       |K|             |                               |
// +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
// |     Extended payload length continued, if payload len == 127  |
// + - - - - - - - - - - - - - - - +-------------------------------+
// |                               |Masking-key, if MASK set to 1  |
// +-------------------------------+-------------------------------+
// | Masking-key (continued)       |          Payload Data         |
// +-------------------------------- - - - - - - - - - - - - - - - +
// :                     Payload Data continued ...                :
// + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
// |                     Payload Data continued ...                |
// +---------------------------------------------------------------+

int make_websocket_frame(WebSocketFrameType frame_type, char* msg, size_t msg_length, char* buffer, size_t &buffer_size, bool copy_only_head=false);
WebSocketFrameType get_websocket_frame( char* in_buffer, int in_length,  char* &out_buffer, int &out_length, int *frame_header_length);
void parse_path(const char *path , int path_len , int &folder_path_length, map<StringPtr, StringPtr, StringPtrCompare>& query_params);

bool answer_web_socket_handshake(SSL *ssl, struct phr_header *websocket_key_header , struct phr_header *websocket_protocol_header, bool is_ssl);

bool send_a_push_notification(const string &gcm_key, const string *node_id,  const string &payload, int type);


class Request{
public:

	SSL *ssl = NULL;

	uint8_t data_io_type = 0; //json or protobuf or else
	map<StringPtr, StringPtr, StringPtrCompare> *headers_map = NULL;
	StringPtr post_data;
	bool is_post_req = false;
	size_t num_headers = 0;
	phr_header headers[100];
	map<StringPtr, StringPtr, StringPtrCompare> query_params;
	string *node_id = NULL;
	bool keep_alive = false;

	Request(){};
	~Request();
	void reset();

	phr_header *get_single_header(const string &header_val);
	StringPtr *get_query_param(const string &query);


	template<typename PROTOOBJ> unique_ptr<PROTOOBJ> get_req_object(){
		if(!is_post_req){
			return nullptr;
		}
		unique_ptr<PROTOOBJ> ret = make_unique<PROTOOBJ>();
		if(ret->ParseFromArray(post_data.ptr, post_data.len)){
			return ret;
		}
		return nullptr;
	}
};





#endif /* HTTP_AND_WEBSOCKET_UTILS_H_ */
