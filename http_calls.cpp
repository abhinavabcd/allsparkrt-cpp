/*
 * http_calls.cpp
 *
 *  Created on: 22-Dec-2016
 *      Author: abhinav
 */


#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <sstream>

#include "boost/shared_ptr.hpp"
#include "boost/move/make_unique.hpp"

#include "wire_messages/wire_transfers.pb.h"

#include "server.h"
#include "http_calls.h"
#include "spdlog/spdlog.h"
#include "ssl_utils.h"
#include "utils.h"
#include "utilities/conversion_utils/conversion_utils.h"
#include "utilities/json/json.hpp"
#include "http_and_websocket_utils.h"
#include "config.h"
#include "constants.h"

using boost::shared_ptr;
using boost::movelib::make_unique;

using allspark::NodeData;
using allspark::SessionNodeData;
using allspark::SessionData;
using allspark::SessionData;
using allspark::BooleanData;
using allspark::StringData;
using allspark::InboxMessagesResponse;
using allspark::GetInboxMessagesRequest;
using allspark::CreateSessionRequest;
using allspark::JoinSessionRequest;
using allspark::SessionsList;

using std::map;
using std::cout;
using std::endl;
using nlohmann::json;

extern std::shared_ptr<spdlog::logger> server_logger;

static string OWNER_CANNOT_UNJOIN = "Owner cannot unjoin the session, instead make somone else owner before unjoining";

extern shared_ptr<Node> current_node;


int error_404(SSL* ssl, const char *content=NULL , int content_length=0){
	char ret[500];//headers + data
	size_t s= 0;
	memcpy(ret,   "HTTP/1.1 404 Not Found\r\n", 24);s+=24;
	memcpy(ret+s ,"Content-Type: text/html\r\n", 25); s+=25;
	memcpy(ret+s, "Content-Length: ", 16);s+=16;
	s += itoa((long int)content_length, ret+s, 10);
	memcpy(ret+s, "\r\n\r\n", 4); s+=4;

	if(content_length >0){
		memcpy(ret+s, content, content_length);s+=content_length;
	}
	write_ssl_auto_handshake(ssl, ret , s, IS_SSL_SERVER);
	return 1;

}


size_t ok_200_headers(char *buff, int n, char *data=NULL, bool is_keep_alive=true){
	size_t s= 0;
	memcpy(buff,  "HTTP/1.1 200 OK\r\n", 17);s+=17;
	memcpy(buff+s , "Content-Type: text/html\r\n", 25); s+=25;
	if(is_keep_alive){
		memcpy(buff+s , "Connection: Keep-Alive\r\n", 24); s+=24;
		memcpy(buff+s , "Keep-Alive: timeout=30\r\n", 24); s+=24;
	}
	memcpy(buff+s, "Content-Length: ", 16);s+=16;
	s += itoa(n, buff+s, 10);
	memcpy(buff+s, "\r\n\r\n", 4); s+=4;
	//copy body
	if(data!=NULL){
		memcpy(buff+s, data , n);
		s+=n;
	}
	return s;
}

inline void add_header(char *buff, char *header_text, char *value_text, size_t &s){
	if(header_text!=NULL){
		size_t l = strlen(header_text);
		memcpy(buff+s, header_text, l);
		s+=l;
	}
	if(value_text!=NULL){
			size_t l = strlen(value_text);
			memcpy(buff+s, value_text, l);
			s+=l;
	}


}

size_t ok_200(SSL *ssl , int n , char *data , bool is_keep_alive = true){
    char ret[n+500];//headers + data
    size_t s = ok_200_headers(ret , n , data, is_keep_alive );
    write_ssl_auto_handshake(ssl, ret, s,  IS_SSL_SERVER);
    return s;
}

size_t ok_200_protobuf(SSL *ssl , ::google::protobuf::Message &message, bool is_keep_alive){
	size_t s= 0;
	size_t n = message.ByteSize();
	char buff[n+500];
	memcpy(buff,  "HTTP/1.1 200 OK\r\n", 17);s+=17;
	memcpy(buff+s , "Content-Type: application/x-protobuf\r\n", 38); s+=38;
	if(is_keep_alive){
		memcpy(buff+s , "Connection: Keep-Alive\r\n", 24); s+=24;
		memcpy(buff+s , "Keep-Alive: timeout=30\r\n", 24); s+=24;
	}

	memcpy(buff+s, "Content-Length: ", 16);s+=16;
	s += itoa(n, buff+s, 10);
	memcpy(buff+s, "\r\n\r\n", 4); s+=4;
	//copy body
	message.SerializeToArray(buff+s, n);
	s+=n;
	write_ssl_auto_handshake(ssl, buff , s ,  IS_SSL_SERVER);
	return s;
}

int websocket_handler_v3(Request *req){
	//read write directly to socket
	shared_ptr<Node> node = db.get_node_by_id(*req->node_id, true, true);
	if(req->get_query_param("get_pre_connection_info")){

		//connect to the same server where he previously connected
		shared_ptr<Node> to_connect = nullptr;
		if(node && !node->is_connected_to_node.empty()){
			to_connect = db.get_node_by_id(node->is_connected_to_node, false, false);
		}
		if(!to_connect){
			//TODO: decide the node to connect to 
			// application layer sharding sharding logic goes here
			//get all select nodes and select based on hash
			to_connect = current_node;
		}

        if(req->data_io_type == PROTOBUF_IO_TYPE){
            NodeData node_data;
            node_data.set_host_address(to_connect->addr);
            node_data.set_port(to_connect->port);
            node_data.set_is_ssl(IS_SSL_SERVER);
            ok_200_protobuf(req->ssl , node_data, req->keep_alive);
        }
        else if(req->data_io_type == JSON_IO_TYPE){
            std::stringstream stream;
            stream << "{\"addr\": \""  << to_connect->addr  <<  "\", \"port\": \"" <<  to_connect->port << "\",\"validation_key\":\"\" , \"is_ssl\": " << (IS_SSL_SERVER?"true":"false") << "}";
            std::stringbuf *pbuf = stream.rdbuf();
            string temp  = pbuf->str();
            ok_200(req->ssl , temp.length() , &temp[0], false );
        }

		return 1;
	}
	return error_404(req->ssl);
}


int get_last_seen_timestamp(Request *req){
	if(req->get_query_param("node_id")){
		long int timestamp = db.get_node_last_ping_received(*req->node_id);
		string resp = std::to_string(timestamp);
		ok_200(req->ssl , resp.length(), &resp[0], true);
		return 1;
	}
	error_404(req->ssl);
	return 1;
}

//user wants to join session or join someone else into the session, guided by sessions  who_can_add_new_nodes flag
int join_session(Request *req){

	shared_ptr<Node> node = db.get_node_by_id(*req->node_id, false, false);
	unique_ptr<JoinSessionRequest> request = req->get_req_object<JoinSessionRequest>();
	if(!request){
			error_404(req->ssl);
			return 1;
	}
	shared_ptr<Session> session = db.get_session_by_id(request->session_id());
	if(session){

		string *to_join_node = !request->node_id().empty()?request->mutable_node_id():req->node_id; //joining self or joining someone else
		int err_code = db.join_session(session->session_id, req->node_id,  to_join_node , request->is_anonymous()  , true, NULL );
		StringData resp;

		if(err_code==SESSION_JOINED){
			shared_ptr<WrappedMessage> msg = mem_recycler.get_shared<WrappedMessage>();
			msg->set_src_id(*req->node_id);
			msg->set_dest_session_id(session->session_id);
			msg->set_type(NEW_NODE_JOINED_SESSION);
			if(request->is_anonymous()){
				msg->set_payload1("anonymous");
			}
			on_message(msg, NULL, 0);//broad cast to all sessin nodes
			resp.set_value("ok");
		}
		else{
			resp.set_value("nope");
		}

		size_t s = ok_200_protobuf(req->ssl, resp, req->keep_alive);
		return 1;
	}
	return error_404(req->ssl);
}

int unjoin_session(Request *req){
	StringPtr *session_id = req->get_query_param("session_id");
	if(session_id){
		shared_ptr<Session> session = db.get_session_by_id(string(session_id->ptr, session_id->len));
		if(session){
			if(session->node_id == *req->node_id){
				//make someone else owner.
				return error_404(req->ssl, &OWNER_CANNOT_UNJOIN[0], OWNER_CANNOT_UNJOIN.length());
			}
			int is_updated = db.unjoin_session(session->session_id, *req->node_id , true);

			if(is_updated){
				shared_ptr<WrappedMessage> msg = mem_recycler.get_shared<WrappedMessage>();
				msg->set_dest_session_id(session_id->ptr, session_id->len);
				msg->set_src_id(*req->node_id);
				msg->set_type(NODE_UNJOINED_SESSION);
				on_message(msg, NULL , 0);//broad cast to all sessin nodes
			}

			char ret[500];
			size_t s = ok_200_headers(ret, 2);
			memcpy(ret+s, "ok", 2); s+=2;
			write_ssl_auto_handshake(req->ssl, ret, s ,  IS_SSL_SERVER);
			return 1;
		}
	}
	return error_404(req->ssl);
}

int reveal_anonymity( Request *req){
	StringPtr *session_id = req->get_query_param("session_id");
	if(session_id){
		shared_ptr<WrappedMessage> msg = mem_recycler.get_shared<WrappedMessage>();//move should be called
		msg->set_src_id(*req->node_id);
		msg->set_dest_id(session_id->ptr, session_id->len);
		msg->set_type(NODE_REVEAL_ANONYMITY);
		on_message(msg, NULL, 0);
		int content_length = 2;
		char ret[500+2];
		size_t s = ok_200_headers(ret, 2);
		memcpy(ret+s, "ok", 2);
		write_ssl_auto_handshake(req->ssl, ret, s,  IS_SSL_SERVER);
		return 1;
	}
	error_404(req->ssl);
}

int get_session_info(Request *req){
	StringPtr *session_id = req->get_query_param("session_id");
	if(session_id){
		string session_id_str  = string(session_id->ptr, session_id->len);
		shared_ptr<Session> session = db.get_session_by_id(session_id_str);
		if(session){
			SessionData session_data;
			session_data.set_session_id(session->session_id);
			session_data.set_node_id(session->node_id);
			session_data.set_description(session->description);
			session_data.set_allow_anonymous(session->allow_anonymous);
			shared_ptr<SessionNodesList> session_nodes = db.get_node_ids_for_session(session_id_str);
			for(map<string, SessionNode>::iterator it = session_nodes->begin();it!=session_nodes->end();it++){
				SessionNodeData* session_node = session_data.add_session_nodes();
				if(it->second.is_anonymous){
					session_node->set_is_anonymous(true);
					session_node->set_node_id(it->second.anonymous_node_id);
				}
				else{
					session_node->set_is_anonymous(false);
					session_node->set_node_id(it->second.node_id);
				}
			}
			ok_200_protobuf(req->ssl, session_data, req->keep_alive);
			return 1;
		}
	}
	error_404(req->ssl);
}


int get_sessions(Request *req){
	shared_ptr<Node> node = db.get_node_by_id(*req->node_id, false, false);
	unique_ptr<SessionsList> sessions_list = req->get_req_object<SessionsList>();
	if(!node || !sessions_list){
		error_404(req->ssl);
		return -1;
	}
	int from = sessions_list->from();
	boost::function<SessionData*()> session_message_allocator =  [&]()->SessionData* {return sessions_list->add_sessions();};
	db.get_sessions_by_node_id(*req->node_id , SESSION_BROADCAST_TYPE_NORMAL, USAGE_TYPE_PLAIN_SESSION , from, &session_message_allocator);
	ok_200_protobuf(req->ssl, (*sessions_list), req->keep_alive);
	return 1;
}

int create_session(Request *req){

	shared_ptr<Node> node = db.get_node_by_id(*req->node_id, false, false);
	unique_ptr<CreateSessionRequest> request = req->get_req_object<CreateSessionRequest>();

	if(!request || !(request->who_can_add_session_nodes()<3 && request->who_can_add_session_nodes()>=0) || !(request->session_type()<2 && request->session_type()>=0)){
		error_404(req->ssl);
		return 1;
	}

	string *description = request->mutable_session_description();
	if(description && description->size()>256){
		description->resize(256);
	}

	//in case of game type session
	string * master_node_id = NULL;
	if(node->is_internal && !request->session_game_master_node_id().empty()){
		master_node_id = request->mutable_session_game_master_node_id();
	}
	else{
		master_node_id = &node->node_id;
	}

	shared_ptr<Session> session = db.create_session(*req->node_id, NULL,  request->session_type(), master_node_id , request->notify_only_last_few_users(), request->who_can_add_session_nodes(), request->is_anonymous(), USAGE_TYPE_PLAIN_SESSION, request->allow_anonymous(), description);
	if(session){
		StringData response;
		response.set_value(session->session_id);
		ok_200_protobuf(req->ssl , response, req->keep_alive);
		return 1;
	}

	error_404(req->ssl);
	return 1;

}



int add_to_inbox(Request *req){
	unique_ptr<Message> msg = req->get_req_object<Message>();
	if(!msg){
		error_404(req->ssl);
		return 1;
	}
	StringPtr *send_notification = req->get_query_param("notify");

	BooleanData resp_obj;

	if(db.add_message_to_inbox(req->node_id, &(*msg))){
		if(send_notification){
			shared_ptr<WrappedMessage> _msg = mem_recycler.get_shared<WrappedMessage>();
			_msg->CopyFrom(*msg);
			on_message(_msg, NULL, 0);
		}
		resp_obj.set_value(true);
	}
	ok_200_protobuf(req->ssl , resp_obj, req->keep_alive);
	return 1;
}



int fetch_inbox_messages(Request * req){
	unique_ptr<GetInboxMessagesRequest> inbox_request = req->get_req_object<GetInboxMessagesRequest>();
	if(!inbox_request){
		error_404(req->ssl);
		return 1;
	}

	string inbox_id;
	const string &node_id = inbox_request->node_id();
	if(!inbox_request->session_id().empty()){
		shared_ptr<Session> session = db.get_session_by_id(inbox_request->session_id());
		if(!session || !db.is_node_in_session(session->session_id, *req->node_id)){
			error_404(req->ssl);
			return 1;
		}
		inbox_id = session->session_id;
	}
	else if(!node_id.empty()){
		shared_ptr<Node> node = db.get_node_by_id(node_id, true, false);
		if(node && !node->is_internal){
			if(node_id < *req->node_id){
				inbox_id = node_id + "_" + (*req->node_id ) ;
			}
			else{
				inbox_id =  (*req->node_id ) +"_"+ node_id ;
			}
		}
	}

	InboxMessagesResponse resp_obj;

	boost::function<InboxMessage*()> msg_allocator  = [&]()->InboxMessage * {return resp_obj.inbox_messages().Add(); };
	bool has_more = false;
	int from_seq = -1;
	int to_seq = -1;
	long int from_timestamp = -1;
	long int to_timestamp = -1;

	unique_ptr<vector<InboxMessage*>> ret = db.fetch_inbox_messages(&inbox_id, has_more, inbox_request->from_seq() , inbox_request->to_seq(), inbox_request->from_timestamp(), inbox_request->to_timestamp(), &msg_allocator);
	resp_obj.set_more(has_more);
	ok_200_protobuf(req->ssl , resp_obj, req->keep_alive);
	return 1;
}


int push_message(Request *req){

	StringPtr *to_nodes = req->get_query_param("to_nodes");

	shared_ptr<Node> node = db.get_node_by_id(*req->node_id, false, false);
	if(!node || !node->is_internal || !req->is_post_req){
		return error_404(req->ssl);
	}
	vector<string> unable_to_send_to;
	if(to_nodes){
	   if(req->data_io_type == PROTOBUF_IO_TYPE){
		allspark::NodesListData nodes_list;
		to_nodes->len = base64_decode( to_nodes->ptr ,  to_nodes->len,  to_nodes->ptr);
		nodes_list.ParseFromArray(to_nodes->ptr , to_nodes->len);
		for(int i=0;i<nodes_list.GetCachedSize();i++){
			shared_ptr<WrappedMessage> msg = mem_recycler.get_shared<WrappedMessage>();

			msg->ParseFromArray(req->post_data.ptr, req->post_data.len);
			msg->set_dest_id(nodes_list.node_id(i));
			on_message(msg, NULL , IS_FROM_HTTP_CALL ,&unable_to_send_to);
			}
		}
	     else if(req->data_io_type == JSON_IO_TYPE){

		urldecode_inplace(to_nodes->ptr, to_nodes->len);
		json nodes_list = json::parse(to_nodes->ptr, to_nodes->ptr + to_nodes->len);
		for(json::iterator it = nodes_list.begin(); it != nodes_list.end(); ++it){

			urldecode_inplace(req->post_data.ptr, req->post_data.len);
			json _message = json::parse(req->post_data.ptr, req->post_data.ptr + req->post_data.len);
			shared_ptr<WrappedMessage> message = mem_recycler.get_shared<WrappedMessage>();
			load_message_from_json(_message, message);
			message->set_dest_id(*it);
			message->clear_dest_session_id();
			on_message(message, NULL, IS_FROM_HTTP_CALL, &unable_to_send_to);
		}
	}

		json resp_json_obj ={
			{"unable_to_send_to" , unable_to_send_to }
		};
		string resp = resp_json_obj.dump();
		char send_buf[resp.length()+ 500];
		ok_200(req->ssl, resp.length(), &resp[0], true);
		return 1;
	}

	error_404(req->ssl);
	return 1;
}





