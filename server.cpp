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
#include <stdlib.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <pthread.h>
#include <map>
#include <vector>
#include <set>
#include <errno.h>
#include <ctime>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>



#define _DEBUG
/*
 * third party libs
 */
#include "libco/co_routine.h"
#include "libco/co_routine_inner.h"

#include "utilities/HMAC_SHA1/SHA1.h"
#include "picohttpparser/picohttpparser.h"

#include "boost/bind.hpp"
#include "boost/bind/placeholders.hpp"
#include "boost/thread.hpp"
#include "boost/regex.hpp"
#include "boost/scoped_ptr.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception_ptr.hpp"


#include "tbb/concurrent_hash_map.h"
#include "tbb/concurrent_queue.h"

/*
 * spdlog logging library
 */
#include "spdlog/spdlog.h"

/*
 * protobuf
 */
#include "wire_messages/wire_transfers.pb.h"
/*
 * application specific
 */
#include "server.h"
#include "mem_utils.h"
#include "mysql_db.h"
#include "http_and_websocket_utils.h"
#include "auth_utils.h"
#include "utils.h"
#include "http_calls.h"
#include "utilities/conversion_utils/conversion_utils.h"
#include "utilities/json/json.hpp"
#include "config.h"
#include "constants.h"
#include "tasks/connection_creator_task.h"
#include "tasks/writer_task.h"
#include "tasks/flush_messages_to_db.h"
#include "utilities/string_hash/murmurhash3.h"


// for convenience
using json = nlohmann::json;

using boost::scoped_ptr;
using boost::movelib::unique_ptr;
using boost::regex;
using std::map;
using std::pair;
using std::make_pair;
using std::vector;
using tbb::concurrent_hash_map;
using std::cout;
using std::endl;
using std::set;
using allspark::InboxMessage;
using std::queue;
using boost::tuple;


#if defined(OPENSSL_THREADS)
  // thread support enabled

MemRecycler mem_recycler;
DatabaseWrapper db;

std::shared_ptr<spdlog::logger> server_logger;
shared_ptr<Node> current_node;

SSL_CTX *server_ssl_ctx;
SSL_CTX *client_ssl_ctx;





static int ssl_session_ctx_id = 1;


/*
 * One flag to rule it all
 */
bool is_server_running = true;
thread_local size_t server_stopped_time = 0;
/*
 * Object properties definitions
 */
void WrappedMessage::recycle(){
	if(data1.buffer_size>0 && data1.ptr!=NULL){
		mem_recycler.recyle(data1.ptr, data1.buffer_size);
		data1.ptr = NULL;
		data1.buffer_size = data1.len = 0;
	}
	if(data2.buffer_size>0 && data2.ptr!=NULL){
		mem_recycler.recyle(data2.ptr, data2.buffer_size);
		data2.ptr = NULL;
		data2.buffer_size = data2.len = 0;
	}
	this->Clear();
}

WrappedMessage::~WrappedMessage(){
	this->recycle();
}

void Connection::init(SSL *ssl,  string &to_node_id , ThreadHandle *thread_handle, bool is_ssl){
    	this->fd = SSL_get_fd(ssl);
    	this->ssl = ssl;
    	this->to_node_id = to_node_id;
    	this->client_type = 0;
		this->last_msg_recv_timestamp = 0;
		this->last_msg_sent_timestamp = this->created_timestamp = get_time_in_ms();
		this->thread_handle = thread_handle;
		this->is_ssl = is_ssl;
		is_external_node = true;
}

void Connection::recycle(){
	while(!pending_queue.empty()){
		server_logger->error("pending queue not empty , should not happen");
		pending_queue.pop();
	}
	while(!msg_assumed_sent.empty()){
		server_logger->error("assumed sent not empty , should not happen usually ");
		msg_assumed_sent.pop();
	}
	thread_handle = NULL;
	last_bytes_read = 0;
	connection_id.clear();
	is_external_node = true;
	last_msg_recv_timestamp = 0;
	last_msg_sent_timestamp = 0;
	created_timestamp = 0;
	client_type = 0;  // 1- raw data publisher  , 2- raw data subscriber
	data_io_type = 0;
	active_writer = NULL;
	ssl = NULL;
}

Connection::~Connection(){
	recycle();
}

MessageTransitToThread::~MessageTransitToThread(){
	recycle();
}

void MessageTransitToThread::recycle(){
	msg = nullptr;
}

void MessageTransitToThread::init(const string &dest_id , shared_ptr<WrappedMessage> &message , size_t fd){
	this->dest_id = dest_id;
	this->msg = message;
	this->fd = fd;
}

ThreadHandle::ThreadHandle() :  messages_queue() , new_sockets() , thread_ref(NULL) , connections_ws(), co_share_stack(NULL){};

//utility functions

void close_socket(int *fd){
	server_logger->info("Closing fd: {}", *fd);
	close(*fd);
}


/*
 * Current node properties
 */
concurrent_hash_map<string, concurrent_queue<Connection*>*> connections;// node_id -> list of connection to it
unique_ptr<IntermediateHopsCache> intermediate_hops(new IntermediateHopsCache(10000));
concurrent_queue<pair< shared_ptr<Node> , shared_ptr<WrappedMessage> > > to_create_connections; // socket_fd -> websocket connection
concurrent_queue<shared_ptr<WrappedMessage>> flush_to_db_queue;
concurrent_queue< tuple<string, shared_ptr<WrappedMessage> , int> > inbox_pending_messages_queue;



/*
 *
 */


bool update_intermediate_node(const string &dest_id, const string &intermediate_node_id){
	if(!intermediate_hops->insert(dest_id, pair<size_t, string>(get_time_in_ms(), intermediate_node_id))){
		return false;
	}
	server_logger->info("Updating intermediate node {}", intermediate_node_id);
	return true;
}




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




/*
 * utility functions end
 */

/*
 * Global variables and constants end
 */


vector<pair<regex, web_request_handler>> web_request_handlers;

/*
 * end HTTP server calls
 */





/*
 * end websocket helpers
 */



/*
 * Step 2
 * most important function that does the basic routing, including intermedaite hop caching.
 * called from Coroutine and multiple threads.
 */
bool place_message_on_sender_thread(shared_ptr<WrappedMessage> &msg, const string *dest_node_id, int message_sending_flags){
	concurrent_hash_map<string, concurrent_queue<Connection*>*>::accessor ac;
	shared_ptr<Node> dest_node = nullptr;
	if(!connections.find(ac , *dest_node_id)){
		goto CHECK_AND_FORWARD_TO_OTHER_NODE;
	}
	concurrent_queue<Connection*> *connection_queue = ac->second;
	Connection *conn , *first_item = NULL;
	bool is_sent_to_live_connection = false;


	//for external node basically forward to all threads
	bool fwd_to_all_node_connections = false;
	dest_node = db.get_node_by_id(*dest_node_id, false, false);

	if(dest_node && !dest_node->is_internal){
		fwd_to_all_node_connections = true; // is closed resending , no need to send to all connections again
	}

	while(connection_queue->try_pop(conn)){//just loop through all items until suitable is found
		//TODO: revisit this , loop through all connections, by making note of the first item ???
		connection_queue->push(conn);
		if(first_item==NULL){
			first_item = conn;
		}
		else if(first_item==conn){
			//all messages iterated through
			break;
		}

		if( (message_sending_flags & IS_RESENDING_ASSUMED_SENT) && conn->is_external_node && conn->created_timestamp < msg->timestamp()){ //send to only those connections that probably missed it, that connected later
			//should already message have in resending queue of this connection, so can ignore
			// as we are broadcasting to all messages
			continue;
		}

		server_logger->info("Placing on sender thread for dest_id: {}", *dest_node_id);
		// CHECK:the shared pointer copies?? if so that should
		shared_ptr<MessageTransitToThread> send_to_thread= mem_recycler.get_shared<MessageTransitToThread>();
		send_to_thread->init(*dest_node_id, msg, conn->fd);
		conn->thread_handle->messages_queue.push(send_to_thread);//copies string onto the pair structure, push it onto the thread that holds the connection
		is_sent_to_live_connection = true;

		//if not fwd to all node_connections you can break
		if(!fwd_to_all_node_connections){
			break;
		}
	}
	if(is_sent_to_live_connection){
		return true;
	}
	CHECK_AND_FORWARD_TO_OTHER_NODE:
		ac.release();//release it
		if((message_sending_flags & USE_DIRECT_CONNECTIONS_ONLY)){
			return false;
		}

		// check if there in something in cache about the dest_id in last 5 minutes if so just place the msg on that connection
		// check in db and check if it exists update intermediate hop and place on that connection.
		IntermediateHopsCache::ConstAccessor ac2;
		if(intermediate_hops->find(ac2, *dest_node_id)){
			const pair<long int , string> *hop = ac2.get();
			long int now = get_time_in_ms();
			if(now - hop->first < 5*60*1000){//created less than 5 minutes ago
				if(place_message_on_sender_thread(msg, &hop->second, USE_DIRECT_CONNECTIONS_ONLY)){//will se if
				    server_logger->info("Using intermediate node {}", hop->second);
				    return true;
				}
			}
			intermediate_hops->remove(ac2);
		}

		//there is not connection or intemediate hop to destination
		//try loading the node from db, reload dest_node_id , see the intermediate node it's connected to
		dest_node = db.get_node_by_id(*dest_node_id, true, true);
		if(!dest_node){
			return false;
		}
		shared_ptr<Node> intermediate_node = nullptr;
		if(dest_node->is_internal){
			intermediate_node = dest_node;
		}
		else{
			intermediate_node  = db.get_node_by_id(dest_node->is_connected_to_node, true, false);
		}

		if(intermediate_node!=nullptr && intermediate_node->node_id!=current_node->node_id){//there is a node that we can send try sending to that node with action level 2=>
			server_logger->info("forwarding to a intermedaite node");
			if(place_message_on_sender_thread(msg, &intermediate_node->node_id, USE_DIRECT_CONNECTIONS_ONLY)){
				update_intermediate_node(*dest_node_id, intermediate_node->node_id);
				return true;
			}
			else{
				//there no no connection to the node , but the destion is connected to cluster
				to_create_connections.push(make_pair(intermediate_node, msg));
				return true;
			}
		}

		return false;

}


/*
 * Step 1
 * Received a new message from a connection,
 * if connection is null , i means an internal transferred message
 * see the message , replicate it to destination ids , pass as a shared_ptr with recycler as deallocator
 * place it on the threadhandler which holds this connection, it will use autoscalable and reusable writer coroutines to disponse data
 *
 *
 * @param: to_session: in case you explicitly want to send to a session
 * @param: is_closed_resending: if this message is emitted from a closed connection , assumed sent buffer
 *
 * @param message_sending_flags  = USE_DIRECT_CONNECTIONS_ONLY  , IS_RESENDING_ASSUMED_SENT  IS_PUSH_TYPE_MESSAGE
 *
 *
*/
void on_message(shared_ptr<WrappedMessage> &msg, Connection* from_conn, int message_sending_flags, vector<string> *unable_to_send = NULL){
	long int now = get_time_in_ms();
	if(from_conn){
		//called from external clients
		from_conn->last_msg_recv_timestamp = now;
		if(from_conn->is_external_node){
			//update message properties appropriately
			msg->set_timestamp(now);
			msg->set_src_id(from_conn->to_node_id);
		}
	}
	vector<const string*> dest;

	if(!msg->dest_id().empty()){//intended for a single user only //dest_id is the first priority over dest_session_id
		dest.push_back(msg->mutable_dest_id());
	}
	else if(!msg->dest_session_id().empty()){//intended to a session
		const shared_ptr<Session> &session = db.get_session_by_id(msg->dest_session_id());
		if(!session){
			return;
		}
		shared_ptr<SessionNodesList> session_nodes = db.get_node_ids_for_session(msg->dest_session_id());
		if(session_nodes==nullptr) return;

		// connection created after it's cached , this check ensures any new member joined before the use and not updated on the server
		if(from_conn && from_conn->created_timestamp > session_nodes->cached_timestamp){
			server_logger->info("refetching session nodes  as the new connection connected after session nodes from cache");
			session_nodes = db.get_node_ids_for_session(msg->dest_session_id(), true);
		}


		map<string, SessionNode>::iterator user_session_node = session_nodes->find(msg->src_id());
		if(user_session_node==session_nodes->end() && msg->type()!=NEW_NODE_JOINED_SESSION){
			//if user is not in session, and it's not joining request notification
			return;
		}

		//check if game type , forward only to master node
		if(session->session_type==SESSION_BROADCAST_TYPE_GAME && session->session_game_master_node_id!=msg->src_id()){
			//forward only to master node id
			dest.push_back(&session->session_game_master_node_id);
		}
		else {
			//broadcast to all nodes in session
			for(map<string, SessionNode>::iterator it= session_nodes->begin();it!=session_nodes->end();it++){
					dest.push_back(&(it->first));
			}
		}
		if(user_session_node->second.is_anonymous){
			msg->set_is_anonymize_src_id(true);
			msg->set_anonymize_src_id(user_session_node->second.anonymous_node_id);
		}

		/*
		 * special handing for session based messages, generated by server
		 */
		if(msg->type()==NEW_NODE_JOINED_SESSION){
				//should not be called from external node
				if(from_conn && from_conn->is_external_node){
					return;
				}
				bool update_in_db = false;
				if(message_sending_flags&IS_FROM_HTTP_CALL){
					update_in_db = true;
				}
				//update in our local set
				db.join_session(msg->dest_session_id(), msg->mutable_src_id(), msg->mutable_src_id(), msg->payload1()=="anonymous", update_in_db, NULL);
		}
		else if(msg->type()==NODE_UNJOINED_SESSION){
			if(from_conn && from_conn->is_external_node){
				return;
			}
			bool update_in_db = false;
			if(message_sending_flags&IS_FROM_HTTP_CALL){
				update_in_db = true;
			}
			db.unjoin_session(msg->dest_session_id(), msg->src_id(),update_in_db);
		}
		else if(msg->type()==NODE_REVEAL_ANONYMITY){
			if(from_conn && from_conn->is_external_node){
				return;
			}
			bool update_in_db = false;
			if(message_sending_flags&IS_FROM_HTTP_CALL){
				msg->set_payload(msg->src_id());
				update_in_db  = true;
			}
			db.reveal_anonymity(msg->dest_session_id(), msg->src_id(), update_in_db);
		}

	}

	else{//special kind of messages websocket messages, mostly config

		if(from_conn && msg->type()==CLIENT_CONFIG_REQUEST){
			//respond with a message with payload that contains messages protobuffed as string
			allspark::ConfigRequest config_request;
			if(from_conn && from_conn->data_io_type == JSON_IO_TYPE){
				load_config_message_from(msg->mutable_payload(), config_request);
			}
			else{
				config_request.ParseFromString(msg->payload());
			}
			allspark::ConfigResponse config_response;
			if(config_request.timestamp()){
				config_response.set_user_time_stamp_diff(now - config_request.timestamp());
			}
			if(!config_request.update_gcm_key().empty()){
				db.update_push_key(msg->src_id(), config_request.update_gcm_key(), 0);
			}
			if(config_request.fetch_inbox_messages()){
				bool has_more = false;
				boost::function<InboxMessage*()> msg_allocator  = [&]()->InboxMessage * {return config_response.mutable_messages()->Add(); };
				unique_ptr<vector<InboxMessage*>> ret = db.fetch_inbox_messages(msg->mutable_src_id(), has_more, config_request.fetch_inbox_messages_from_seq(), config_request.fetch_inbox_messages_to_seq(), config_request.fetch_inbox_messages_from_timestamp(), config_request.fetch_inbox_messages_to_timestamp(), &msg_allocator);
				config_response.set_more(has_more);
			}
				// reuse the same message to forward
			if(from_conn && from_conn->data_io_type==JSON_IO_TYPE){
				msg->set_payload(load_config_response_to_json(config_response));
			}
			else{
				config_response.SerializeToString(msg->mutable_payload());
			}
			msg->set_dest_id(msg->src_id());
			msg->set_type(CLIENT_CONFIG_RESPONSE);
			msg->clear_src_id();
				//connection is held by current thread , so just place it directly on its pending queue
			from_conn->pending_queue.push(std::move(msg));
			if(!from_conn->active_writer){
				//force start a writer task
				start_writer_task(from_conn, from_conn->thread_handle);
			}
			return;
		}
	}


	//serialize the data after the necessary transformations , make it ready to send it to clients
	// this is an optimization , where we serialize it only once , instead serialing for each node.
	if(dest.size()>0 && !(message_sending_flags&IS_RESENDING_ASSUMED_SENT)){//renseind implies this has been already done
		//serialize and save the buffers
		if(!msg->data1.ptr){
			msg->data1.ptr  = mem_recycler.get(_2KB_);
			msg->data1.buffer_size  = _2KB_;
			msg->data1.len = msg->ByteSize();
			msg->SerializePartialToArray(msg->data1.ptr, msg->data1.buffer_size);
		}
		if(msg->is_anonymize_src_id() && !msg->data2.ptr){
			msg->data2.ptr  = mem_recycler.get(_2KB_);
			msg->data2.buffer_size  = _2KB_;
			string restore_src_id(msg->src_id());

			msg->set_src_id(msg->anonymize_src_id());
			msg->data2.len = msg->ByteSize();
			msg->SerializePartialToArray(msg->data2.ptr, msg->data2.buffer_size);
			//restore original src_id ,
			msg->set_src_id(restore_src_id);
		}
	}

	for(vector<const string*>::iterator it = dest.begin();it!=dest.end();it++){
		if(*(*it)==msg->src_id()) continue;//don't send to self

		bool is_delegated_to_sender_thread = place_message_on_sender_thread(msg, (*it), message_sending_flags);

		if(!is_delegated_to_sender_thread){
			inbox_pending_messages_queue.push(tuple<string, shared_ptr<WrappedMessage> , int>(*(*it), msg , message_sending_flags));//copies to pair and moves to queue
			if(unable_to_send){
				unable_to_send->push_back(*(*it));
			}
		}
		//push onto a db updating queue and save all messages into db
		if(msg->type() <= 0){ //all unimportant messges
			continue;
		}
	}
	//sync all messages into queues
	if(server_config.config_enable_flush_all_messages_to_db && (( message_sending_flags & IS_FROM_HTTP_CALL) || (from_conn!=NULL && from_conn->is_external_node) ) && msg->type()>0){
		flush_to_db_queue.push(std::move(msg));
	}

}

/*
 * initialize a new connection
 * Called from a coroutine
 *
 */
Connection *track_new_connection(SSL *ssl , shared_ptr<Node> &to_node, ThreadHandle *thread_handle, bool is_ssl){
	Connection *conn = mem_recycler.get<Connection>();
	conn->init(ssl , to_node->node_id, thread_handle, is_ssl);
	conn->is_external_node = !to_node->is_internal;

	conn->should_save_resending_buffer = true;
	concurrent_hash_map<string, concurrent_queue<Connection*>*>::accessor ac;

	bool is_first_connection_by_node = false;

	if(!connections.find(ac , to_node->node_id)){
		connections.insert(ac , to_node->node_id);//should copy to the connections map
		ac->second = new concurrent_queue<Connection *>();
		is_first_connection_by_node = true;
	}
	//should not be greater than 50 connections from a perticular node
	if(conn->is_external_node && ac->second->unsafe_size()>50){
		mem_recycler.recycle(conn);
		server_logger->error("tracking greater than 50 connections. not allowing {}", to_node->node_id);
		return NULL;
	}
	ac->second->push(conn);
	ac.release();

	if(is_first_connection_by_node && !to_node->is_internal){
		db.set_is_connected_to_node(to_node->node_id , &current_node->node_id);
	}

	map<int, Connection*>::iterator iter = thread_handle->connections_ws.find(conn->fd);
	if(iter != thread_handle->connections_ws.end()){
		server_logger->error("tracking an already existing untracked connection..");
	}
	thread_handle->connections_ws.insert(iter, make_pair(conn->fd, conn));
	return conn;
}
/*
 * flush pending messages and messages sent in last TCP_USER_TIMEOUT buffer
 * Called from a coroutine
 */
void untrack_connection(Connection *conn, bool resend_last_messages, ThreadHandle* thread_handle){//always resend last few messages

	resend_last_messages = resend_last_messages || conn->last_bytes_read<0; // bytes_read<0 implies socket closed with error


	map<int, Connection*>::iterator iter = thread_handle->connections_ws.find(conn->fd);
	if(iter!=thread_handle->connections_ws.end() && iter->second==conn){
		// found connection for fd
		thread_handle->connections_ws.erase(iter);
	}
	else{
		server_logger->error("Darn! There is no local map for the node_id");
	}

	concurrent_hash_map<string, concurrent_queue<Connection*>*>::accessor ac;
	if(!connections.find(ac , conn->to_node_id)){
		server_logger->error("Darn! There is no local map for the node_id");
		mem_recycler.recycle(conn);
		return;
	}

	concurrent_queue<Connection*> *v = ac->second;
	Connection *conn_temp = NULL;
	int retries = 0;
	server_logger->info("untracking connection {}",conn->to_node_id);

	Connection *already_seen = NULL;
	while(v->try_pop(conn_temp)){//assuming all concurrent modifications
		if(conn_temp==conn){
			break;
		}
		v->push(conn_temp);//push back at the end again this also help in load balancing among multiple connections
		if(!already_seen){
			already_seen = conn_temp;
		}
		else if(already_seen==conn_temp){
			//we are looping again
			break;
		}

		retries++;
		server_logger->info("looping through client connections {}", retries);
	}
	if(v->empty()){
		if(connections.erase(ac)){
			delete v;
			db.set_is_connected_to_node(conn->to_node_id, NULL);
		}
	}
	ac.release();
	while(!conn->pending_queue.empty()){
		shared_ptr<WrappedMessage> &msg = conn->pending_queue.front(); // not sure of this

        on_message(msg, NULL , 0);// will be moved
		server_logger->info("resending pending messages ");
		msg = nullptr;
		conn->pending_queue.pop();
	}

//	tuple<long int , Message*> assumed_sent_queued_msg;
	size_t now = get_time_in_ms();
    while(resend_last_messages && !conn->msg_assumed_sent.empty()){
    	pair<size_t , shared_ptr<WrappedMessage>> &assumed_sent_msg = conn->msg_assumed_sent.front();
    	size_t timestamp = assumed_sent_msg.first;
    	shared_ptr<WrappedMessage> &msg = assumed_sent_msg.second;
    	if (!(msg->type()==CLIENT_CONFIG_RESPONSE || msg->type()==0 || msg->type()==USER_OFFLINE_RESPONSE) &&  (now - timestamp < _MAX_TCP_USER_TIMEOUT*1000)){
    		server_logger->info("resending last messages ");
    		on_message(msg, NULL , IS_RESENDING_ASSUMED_SENT | USE_DIRECT_CONNECTIONS_ONLY);//place on sender thread or just into db and send push notifications, this should copy the shared ptr, we are going to remove it from here, TODO: make sure
    	}
    	else{
    		msg = nullptr; // explicit destroy
    	}
    	conn->msg_assumed_sent.pop();//loose it
    }
    mem_recycler.recycle(conn);
}



/*
 * should be called from a co routine
 */
void handle_connection(Connection *conn, ThreadHandle *thread_handle){
	set_timeouts(conn->fd, 40 ,10);//set read timeout on socket to indefinite until close
	size_t message_buff_len  = 0;
	size_t mem_size  = 4*1024;//32Kb

	int n_messages_total = 0;
	int n_messages_last_min = 0;
	size_t cur_minute = -1;
	int n_warnings = 0;
	unique_ptr<char , boost::function<void(void *)>> message_buff = unique_ptr<char , boost::function<void(void *)>>( (char*)mem_recycler.get(mem_size) , boost::bind(&MemRecycler::recyle, &mem_recycler, boost::placeholders::_1, mem_size));
	ssize_t &bytes_read = conn->last_bytes_read;
	while (true) {
		bytes_read = read_ssl(conn->ssl,message_buff.get()+message_buff_len, mem_size-message_buff_len, conn->is_ssl);
		if(bytes_read<0){			//20 min send ping everytime.
			long int now = get_time_in_ms();
			if(errno==LIBCO_POLL_TIMEOUT){
				//check for external node if we didn't send any message in ping_interval minutes, send probe now to test if connection is still alive
				if(now - conn->last_msg_sent_timestamp > conn->ping_interval  && conn->is_external_node){
					//send a ping, onto the same connection.
					shared_ptr<WrappedMessage> empty_message = mem_recycler.get_shared<WrappedMessage>();
					conn->pending_queue.push(std::move(empty_message));
					if(!conn->active_writer){
						//force start a writer task
						start_writer_task(conn, thread_handle);
					}
				}
				//for internal nodes , check if no message in 20 min in either directions , close the connection
				if(!conn->is_external_node && now - conn->last_msg_sent_timestamp > 20*60*1000 && now - conn->last_msg_recv_timestamp > 20*60*1000){
					//no message in either directions in 20 minutes, then you may wish to close this connection
					break;
				}
				if(!is_server_running){
					break;
				}
				continue;
			}
		}


		if (bytes_read <= 0){//file descriptor closed with not EAGAIN
			break;
		}


		message_buff_len+=bytes_read;
		if(mem_size-message_buff_len < 100){
			//reallocate and copy stuff
		}
		bool close_connection = false;
		while(true){
			char *message_data;
			int frame_header_length = 0;
			int message_data_length =0;
			WebSocketFrameType type = get_websocket_frame(message_buff.get(), message_buff_len, message_data, message_data_length, &frame_header_length);

			if(type==CLOSING_FRAME){
				char closing_frame[10];
				size_t temp = 0;
				make_websocket_frame(CLOSING_FRAME, NULL , 0, closing_frame, temp, true);
				write_ssl_auto_handshake(conn->ssl, closing_frame, temp, conn->is_ssl);
				close_connection = true;
				break;
			}
			else if(type ==TEXT_FRAME || type== BINARY_FRAME){
				shared_ptr<WrappedMessage> message = mem_recycler.get_shared<WrappedMessage>();
				if(conn->data_io_type==PROTOBUF_IO_TYPE){
					//almost like emitting message from a connection
					if(!message->ParseFromArray(message_data, message_data_length)){
						close_connection = true;
						break;
					}
				}else if(conn->data_io_type==JSON_IO_TYPE){
					//convert to message object
					json j = json::parse(message_data, message_data+message_data_length);
					load_message_from_json(j, message);
				}


				if(conn->is_external_node){
					long int now = get_time_in_ms();
					if(now/(1000*60) == cur_minute){
						if(++n_messages_last_min > 200){//rate of messages
							server_logger->error("high rate of messages from {}", conn->to_node_id);
							if(++n_warnings > 5){
								close_connection = true;
								break;
							}
						}
					}
					else{
						n_messages_last_min = 0;
						cur_minute =  now/(1000*60);
					}
				}
				n_messages_total++;
				on_message(message, conn , 0);
				if(conn->is_external_node){
					//update last msg received time stamp
					db.set_node_last_ping_received(conn->to_node_id, conn->last_msg_recv_timestamp);
				}
				message_buff_len -= (frame_header_length + message_data_length);
				if(message_buff_len>0){
					//copy remaining data
					memcpy(message_buff.get(), message_buff.get()+frame_header_length+message_data_length, message_buff_len);
					continue;
				}
			}
			else if(type==PING_FRAME){
				//send a pong frame
				char pong_frame[10];
				size_t temp = 0;
				//no pong frame , sorry , instead applying warm water to the injured area is sending an empty message
				shared_ptr<WrappedMessage> empty_message = mem_recycler.get_shared<WrappedMessage>();
				conn->pending_queue.push(std::move(empty_message));
				if(!conn->active_writer){
					//force start a writer task
					start_writer_task(conn, thread_handle);
				}
			}
			//break if complete frame not found, loop again to read more data
			break;
		}
		if(close_connection){
			break;
		}
	}
}

/*
 * reader task coroutine
 */

static void *reader_task(void *_arg){
	//sample the protocol, read the header data, read the auth key, identify connection as json/protobuff, create the connection block.
	//all data be created on stack
	co_enable_hook_sys();
	ReaderTask *task = _arg;


	while(true){

		int fd = task->fd;
		ThreadHandle * thread_handle = task->thread_handle;

		if(fd==-1){
			thread_handle->reader_tasks_pool.push(task);//put back into pool and sleep
			co_yield_ct();
			continue;
		}

		allspark::AuthData auth_obj;//one per reader task enough
		Request req;

		unique_ptr<SSL , boost::function<void(SSL *)> > ssl_fd_auto_close = nullptr;
		unique_ptr<char , boost::function<void(void *)> > header_buff = nullptr;
		unique_ptr<Connection , boost::function<void(Connection*)> > conn = nullptr;

		char *method, *path;
		int header_data_processed_len, minor_version;
		shared_ptr<Node> node = nullptr;
		//make non blocking read/write
		//keep reading data until validity , if error , simply close socket any where.
		//read the http headers..

		try{
			SSL* ssl = req.ssl =  SSL_new(server_ssl_ctx);
			if(!ssl || !SSL_set_fd(ssl, fd)){
				goto CLOSE;
			}

			ssl_fd_auto_close = unique_ptr<SSL , boost::function<void(SSL *)> >( ssl , boost::bind(&close_ssl, boost::placeholders::_1, fd ));//auto close on loop exit

			header_buff = unique_ptr<char , boost::function<void(void *)> >((char *)mem_recycler.get(2048) , boost::bind(&MemRecycler::recyle, &mem_recycler, boost::placeholders::_1, 2048));



			//if you release prematurely , don't forget to reset
			//make and accept the handshake..
			if(IS_SSL_SERVER){
				int ssl_accept_ret  = 0;
				while ( (ssl_accept_ret = SSL_accept(ssl)) <= 0) {
					if(ssl_accept_ret< 0 && errno!=LIBCO_POLL_TIMEOUT){
						int ssl_err = SSL_get_error(ssl,ssl_accept_ret);
						server_logger->error("SSL_ACCEPT_ERROR  {}", ERR_error_string(ERR_get_error(), NULL));
						if(ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ){
							// in the process
							continue;
						}
					}
					goto CLOSE;
				}
			}

			KEEP_ALIVE_JUMP:
			//reset data counters
			size_t num_header_bytes = 0, prevbuflen = 0, method_len, path_len;
			size_t post_data_len = 0;
			req.reset();
			/*
			 * read the headers of request
			 */
			while (true) {
				/* read the request */
				ssize_t bytes_read = read_ssl(ssl, header_buff.get()+num_header_bytes, 2048-num_header_bytes , IS_SSL_SERVER);
				if (bytes_read <= 0){
					goto CLOSE;
				}
				prevbuflen = num_header_bytes;
				num_header_bytes+=bytes_read;
				/* parse the request */
				req.num_headers = sizeof(req.headers) / sizeof(req.headers[0]);
				header_data_processed_len = phr_parse_request(header_buff.get(), num_header_bytes, (const char **)&method, &method_len, (const char **)&path, &path_len,
										 &minor_version, req.headers, &(req.num_headers), prevbuflen);
				if (header_data_processed_len > 0)
					break; /* successfully parsed the request */
				else if (header_data_processed_len == -1){
					goto CLOSE;
				}
				if (num_header_bytes >= 2048){
				   goto CLOSE;
				}
				//pret == -2 continue looping.
			}
			//understand the headers
			struct phr_header *websocket_key_header = req.get_single_header("Sec-WebSocket-Key");
			struct phr_header *websocket_protocol_header = req.get_single_header("Sec-WebSocket-Version");
			struct phr_header *content_length_header = req.get_single_header("Content-Length");
			struct phr_header *keep_alive = req.get_single_header("connection");




			if(strncmp(method, "POST", method_len)==0 && content_length_header!=NULL){
				server_logger->info("post mesage received");
				post_data_len = atoi(content_length_header->value, content_length_header->value_len);
				while(num_header_bytes - header_data_processed_len < post_data_len && num_header_bytes < 2048){
					ssize_t bytes_read = read_ssl(ssl , header_buff.get()+num_header_bytes , 2048-num_header_bytes, IS_SSL_SERVER);
					if(bytes_read <= 0 ){
						goto CLOSE;
					}
					num_header_bytes+=bytes_read;
				}

				req.is_post_req = true;
				req.post_data.ptr = header_buff.get()+header_data_processed_len;
				req.post_data.len = post_data_len;
			}

			//parse path and query params from the request
			int folder_path_len = 0;//until '?' in the path
			parse_path(path , path_len,  folder_path_len , req.query_params);


			StringPtr *auth_key_ptr = req.get_query_param("auth_key");
			if(auth_key_ptr==NULL){
				server_logger->error("no auth key");
				goto CLOSE;
			}

			StringPtr *data_type = req.get_query_param("data_type");
			if(data_type && strncmp("protobuf",data_type->ptr, data_type->len)==0){
				req.data_io_type = conn->data_io_type = PROTOBUF_IO_TYPE;
			}

			/*
			 * validate the connection , should be encrypted with a key
			 */
			urldecode_inplace(auth_key_ptr->ptr , auth_key_ptr->len);//decode auth_key from query_params
			if(!retrieve_auth(auth_key_ptr->ptr , auth_key_ptr->len , auth_obj)){
				server_logger->error("Invalid auth_key {}", string(auth_key_ptr->ptr, auth_key_ptr->len));
				goto CLOSE;
			}


			req.node_id = auth_obj.mutable_node_id();

			/*
			 * authenticated and valid node
			 */
			if(!websocket_key_header){
				// must be http , return response directly

				/*
				 * TODO: parse and handle http here
				 */
				if(keep_alive!=NULL && strncasecmp(keep_alive->value,"keep-alive" ,10)==0){
					req.keep_alive = true;
				}
				for(vector<pair<regex, web_request_handler>>::iterator it = web_request_handlers.begin();it!=web_request_handlers.end();it++){
					if(boost::regex_search( path , path+path_len,  it->first)){
						web_request_handler &handler = it->second;
						handler(&req);
						break;
					}
				}
				if(req.keep_alive){
					server_logger->info("keeping alive");
					set_timeouts(fd, 40 ,5);//set timeout of 40 seconds
					goto KEEP_ALIVE_JUMP;
				}
				goto CLOSE;
			}


			/*
			 * answer websocket handshake
			 */

			if(!answer_web_socket_handshake(ssl, websocket_key_header, websocket_protocol_header, IS_SSL_SERVER)){
				server_logger->error("Invalid handshake");
				goto CLOSE;
			}

			//handle websocket
			node = db.get_node_by_id(*auth_obj.mutable_node_id(), false, false);
			if(!node){
				server_logger->error("couldn't retrieve node from db {}", auth_obj.node_id());
				goto CLOSE;
			}

			//create a new connection and register it globally
			Connection *conn_temp =  track_new_connection(ssl, node, thread_handle, IS_SSL_SERVER);
			if(!conn_temp){
				server_logger->error("couldn't track connection");
			     goto CLOSE;
			}
			conn = unique_ptr<Connection , boost::function<void(Connection*)>>(conn_temp , boost::bind(&untrack_connection , boost::placeholders::_1, true, thread_handle));
			conn->client_type = auth_obj.client_type();


			StringPtr *ping_interval_time = req.get_query_param("ping_interval_millis");
			if(ping_interval_time){
				int ping_interval = atoi(ping_interval_time->ptr, ping_interval_time->len) > 0 ;
				conn->ping_interval = std::max(ping_interval, 2*60*1000);//no less than 2 minutes
			}


			handle_connection(conn.get(), thread_handle);

			CLOSE:; //just an empty statement
			if(conn)
				server_logger->info("closing connction {} ", conn->to_node_id);

		}
		catch(sql::SQLException &ex){
			server_logger->error("mysql error {} {} line {}" ,ex.getErrorCode() , ex.what() ,__LINE__);
		}
		catch(std::invalid_argument &ex){
			server_logger->error("json parse error {} line {}" ,ex.what() , __LINE__);
		}
		//reset task
		task->fd = -1;
//		RECYCLE_HEADER_AND_NO_CLOSE:
//			mem_recycler.recyle(header_buff, 2048);
//			continue;
	}
}




void start_reader_task(ThreadHandle * thread_handle, int fd){
	ReaderTask *ret;
	//set the socket options
	if(thread_handle->reader_tasks_pool.empty()){
		//try creating new writer task
		ret = new ReaderTask();

		stCoRoutineAttr_t attr;
		attr.stack_size = 0;
		attr.share_stack = thread_handle->co_share_stack;

		co_create(&ret->co_routine ,&attr, reader_task , ret);
	}
	else{
		ret = thread_handle->reader_tasks_pool.front();
		thread_handle->reader_tasks_pool.pop();
	}
	ret->fd = fd;
	ret->thread_handle = thread_handle;
	co_resume(ret->co_routine);
}




int on_each_event_loop(void * arg){//executes at min 1 sec interval (should be less if there is activity on the sockets owned by thw thread

	ThreadHandle *thread_handle = reinterpret_cast<ThreadHandle*>(arg);

	shared_ptr<MessageTransitToThread> to_send = nullptr;
	while(thread_handle->messages_queue.try_pop(to_send)){//to be handled by this thread only, else use db and transfer futher, if no route , keep in db
		//write_to_the_connection queue data
		//delegate connection objects to handlers and set flags on the connections that they have pending write on them and
		//handle appropriately mostly single threaded mode( yayy!! : ) )

		map<int, Connection*>::iterator iter = thread_handle->connections_ws.find(to_send->fd);
		if(iter == thread_handle->connections_ws.end()){
			// connection no more exists place it back again on queue
			//TODO: danger call , move it to a co routine as this could block the main loop
			server_logger->error("message placed on thread but doesnt handle fd");
			on_message(to_send->msg, NULL, 0);
			continue;
		}
		Connection *conn = iter->second;
		if(conn->thread_handle!=thread_handle){
			//wtf !! placed on wrong thread , do something about it, add err log!
			server_logger->error("message placed on incorrect thread, can never happen here!");
			on_message( to_send->msg, NULL, 0); //places onto other thread if possible with a different connection
			continue;
		}

		conn->pending_queue.push(std::move(to_send->msg));

		if(conn->active_writer!=NULL || conn->fd==-1){//already writer is active
			continue;
		}
		//grab a worker thread and delegate task to that
		start_writer_task(conn, thread_handle);
	}

	//spawn new connection to other reachable nodes
	pair< shared_ptr<Node> , shared_ptr<WrappedMessage> > msg_pending_for_new_connection;
	while(to_create_connections.try_pop(msg_pending_for_new_connection)){
		server_logger->info("need to create new connections");
		start_connection_creator_task(msg_pending_for_new_connection.first, msg_pending_for_new_connection.second, thread_handle);
	}

	//handle new sockets
	int fd = 0;
	while(thread_handle->new_sockets.try_pop(fd)){
		server_logger->info("new connection {} ", fd);
		start_reader_task(thread_handle, fd);
	}



//	string node_id;
//	set<string> temp_s;
//	while(need_more_connections.try_pop(node_id)){//automatically should load distribute
//		if(temp_s.find(node_id)!=temp_s.end()){
//			continue;
//		}
//		temp_s.insert(node_id);
//		pair<stCoRoutine_t* , string>*task = get_connection_creator_task();
//		task->second = node_id;
//		co_resume(task->first);
//	}
	if(!is_server_running){
		if(thread_handle->connections_ws.size()==0 && get_co_routines_count()<1){ //all coroutines ended, those waiting on poll and stuff
				return -1;
		}
	}

	return 0;//OK
}



/*
 * intializes the server thread with a thread handle
 */
void *init_server_thread(void *arg){
	//TODO: multi threaded , accepts connections and delegates to the threads
	ThreadHandle *thread_handle = arg;
	//1000 sockets handled by one shared stack , we are creating 100 of them
	thread_handle->co_share_stack= co_alloc_sharestack(100, 1024 * 128);

	stCoEpoll_t * ev = co_get_epoll_ct(); //ct = current thread
	co_eventloop( ev,on_each_event_loop, thread_handle );//this will wake up all sockets that have something to read/write to
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

/*
 * TODO: add sigkill handler, that should set is server running false
 */

/*
 * link from co_hoosk_sys_call;
 */
void signal_callback_handler(int signum){
   server_logger->error("Caught signal {}",signum);
   if(signum==2 || signum==15){
	   is_server_running = false;
       close(g_listen_fd);// close the listenening socket accept no more
   }
}

int main(int argc,char *argv[]){

	load_config_from_json("config.json");
	db.init();


    try {
        spdlog::set_async_mode(524288);

        std::vector<spdlog::sink_ptr> sinks;
#ifndef VERSION_RELEASE
		sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
#endif
		std::shared_ptr<spdlog::sinks::daily_file_sink_mt>daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logs/log", "txt", 0, 0);
        sinks.push_back(daily_sink);

        server_logger = std::make_shared<spdlog::logger>("server_logger", begin(sinks), end(sinks));
        server_logger->set_pattern("[%Y%m%d %H:%M:%S.%e] [%t] [%l] %v");
    }
    catch (const spdlog::spdlog_ex& ex){
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
        return 1;
    }


    server_logger->info("Starting server");


	std::srand (std::time (0));
    signal(SIGINT, signal_callback_handler);
    signal(SIGTERM, signal_callback_handler);
    signal(SIGPIPE, SIG_IGN);//ignore the sigpipe , write broken error and just handle with a return value of -1


    // init open ssl

    server_ssl_ctx = init_openssl_server();
    client_ssl_ctx = init_openssl_client();

	// now start a socket server and accept connections
	// delegate them to the threads , by putting them on the thread handlers in round robin fashion

	unsigned int nthreads = boost::thread::hardware_concurrency();
	if(nthreads==0){
		nthreads = atoi( argv[3] );
	}
	pthread_t tid[ nthreads ];
	ThreadHandle thread_handles[nthreads];

	for(unsigned int i=0;i<nthreads;i++){
		ThreadHandle * handle = thread_handles+i;
		pthread_create( tid + i,NULL, init_server_thread,handle);
		handle->thread_ref = tid + i;
	}
	pthread_t db_message_flush_thread = start_flush_to_db_thread();


	web_request_handlers.push_back(make_pair(regex("^/connectV3"), &websocket_handler_v3));
	web_request_handlers.push_back(make_pair(regex("^/join_session"), &join_session));
	web_request_handlers.push_back(make_pair(regex("^/unjoin_session"), &unjoin_session));
	web_request_handlers.push_back(make_pair(regex("^/reveal_anonymity"), &reveal_anonymity));
	web_request_handlers.push_back(make_pair(regex("^/get_session_info"), &get_session_info));
	web_request_handlers.push_back(make_pair(regex("^/create_session"), &create_session));
	web_request_handlers.push_back(make_pair(regex("^/push_message"), &push_message));
	web_request_handlers.push_back(make_pair(regex("^/last_seen"), &get_last_seen_timestamp));


	web_request_handlers.push_back(make_pair(regex("^/get_sessions"), &get_sessions));



	web_request_handlers.push_back(make_pair(regex("^/add_to_inbox"), &add_to_inbox));
	web_request_handlers.push_back(make_pair(regex("^/fetch_inbox_messages"), &fetch_inbox_messages));

	string ip;
	string port_str(argv[1]);
	if(argc< 2){
	    server_logger->error("No of arguments less than expected");
	    goto CLOSE_SERVER;
	}
	int port = atoi( argv[1] );

	if(argc<=2){
		//use aws config
		//curl http://169.254.169.254/latest/meta-data/public-ipv4
	}
	else{
		ip = argv[2];
	}

	server_logger->info("Ip: {}, Port: {}, ", ip, port);

	//initialize current node


	g_listen_fd = create_tcp_socket( port,"*",true );
	if(listen( g_listen_fd, 1024 )==-1){//listen with a backlog of 1024 sockets
	    server_logger->error("Listening to sockets failed");
	    goto CLOSE_SERVER;
	}
	unsigned int tcp_max_user_timeout  = (_MAX_TCP_USER_TIMEOUT - 20) * 1000;
    if (setsockopt (g_listen_fd, SOL_TCP, TCP_USER_TIMEOUT, (char *)&tcp_max_user_timeout,
                sizeof(tcp_max_user_timeout)) < 0){
    	server_logger->error("cannot set tcp max timeout");
    }




	if((current_node = db.node_config_exists(ip , port_str))==nullptr){
		current_node = db.create_node(ip , port_str, true, IS_SSL_SERVER);
	}

	int _new_socket_handler_index = 0;
	set_non_blocking(g_listen_fd);
	while(is_server_running){
		struct sockaddr_in addr; //maybe sockaddr_un;
		memset( &addr,0,sizeof(addr) );
		socklen_t len = sizeof(addr);
		int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
		if(fd<0 && errno!=EAGAIN){
			is_server_running = false;
			break;
		}
		if(fd<0){
			struct pollfd pf = { 0 };
			pf.fd = g_listen_fd;
			pf.events = (POLLIN|POLLERR|POLLHUP);
			poll(&pf,1,25000 );
			continue;
		}

		//keep distributing to threads in round robin way
		thread_handles[_new_socket_handler_index].new_sockets.push(fd);
		_new_socket_handler_index = (_new_socket_handler_index+1)%nthreads;
	}
	//you no longer accept new connections.
	CLOSE_SERVER:
	server_logger->info("Closing the listening socket");
	is_server_running = false;



	//flush all messages and then exit the server
	//close all socket fd , forcing either flush to db or writing to remote ends
	//join all threads

	void *thread_ret;
	pthread_join(db_message_flush_thread, &thread_ret);
	for(unsigned int i=0;i<nthreads;i++){
		pthread_join(tid[i], &thread_ret);
	}
	server_logger->info("closing connection handling threads");



	close(g_listen_fd);// close the listening socket accept no more

	mem_recycler.free_all_pools();
	db.close_all_connections();
	//clear caches
	db.clear_all_caches();
	intermediate_hops->clear();

	//free all co routines
	//free all new objects
	for(unsigned int i=0;i<nthreads;i++){
		while(!thread_handles[i].reader_tasks_pool.empty()){
			ReaderTask *reader_task = thread_handles[i].reader_tasks_pool.front();
			co_free(reader_task->co_routine);
			free(reader_task);
			thread_handles[i].reader_tasks_pool.pop();
		}
		while(!thread_handles[i].writer_tasks_pool.empty()){
					WriterTask *task = thread_handles[i].writer_tasks_pool.front();
					co_free(task->co_routine);
					free(task);
					thread_handles[i].writer_tasks_pool.pop();
		}
		while(!thread_handles[i].connection_creator_tasks_pool.empty()){
					NewConnectionCreatorTask *task = thread_handles[i].connection_creator_tasks_pool.front();
					co_free(task->co_routine);
					free(task);
					thread_handles[i].connection_creator_tasks_pool.pop();
		}

		for(int j=0;j<thread_handles[i].co_share_stack->count;j++){
			free(thread_handles[i].co_share_stack->stack_array[j]->stack_buffer);//stack buff
			free(thread_handles[i].co_share_stack->stack_array[j]);//stackmem
		}
		free(thread_handles[i].co_share_stack->stack_array);//stackmem
		free(thread_handles[i].co_share_stack);
	}

	cleanup_openssl(server_ssl_ctx);
	cleanup_openssl(client_ssl_ctx);
	spdlog::drop_all();
	return 0;
}

#else
  // no thread support
	exit(1);
#endif

/*

abhinav
sindhu
hello
hello2

gRI3tkBR9rZeuBUSoxnSBm7LvdCBCQoHYWJoaW5hdg==
gRJXq7bH3GwB4azf9zOM2IXBSgWBCAoGc2luZGh1
gRLxyoBs0BPhxVDqxlJOKz2nz5GBBwoFaGVsbG8=
gRILNJnAiwQQollQnFs+jb9e7oiBCAoGaGVsbG8y

*/
