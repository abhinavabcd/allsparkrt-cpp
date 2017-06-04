/*
 * server.h
 *
 *  Created on: Dec 4, 2016
 *      Author: abhinav
 */

#ifndef SERVER_H_
#define SERVER_H_

#include "libco/co_routine.h"
#include "libco/co_routine_inner.h"
#include <sys/time.h>
#include "tbb/concurrent_queue.h"
#include "tbb/concurrent_hash_map.h"
#include <vector>
#include <map>
#include <queue>
#include "wire_messages/wire_transfers.pb.h"
#include "picohttpparser/picohttpparser.h"
#include "boost/shared_ptr.hpp"
#include "boost/tuple/tuple.hpp"
#include "utils.h"
#include "http_and_websocket_utils.h"


#include "utilities/json/json.hpp"
#include "boost/function.hpp"
#include "ssl_utils.h"
#include "utilities/concurrent_lru_cache.h"
#include "boost/container/deque.hpp"

using std::map;
using std::string;
using tbb::concurrent_queue;
using tbb::concurrent_hash_map;
using allspark::Message;
using boost::tuple;
using std::pair;
using std::vector;
using std::queue;
using boost::shared_ptr;
using std::queue;
using json = nlohmann::json;
using boost::shared_ptr;
using boost::function;



#define _2KB_ 2048

#define JSON_IO_TYPE 0
#define PROTOBUF_IO_TYPE 1



#ifdef SSL_SERVER
#define IS_SSL_SERVER true
#else
#define IS_SSL_SERVER false
#endif



class WrappedMessage:public Message{
public:

	static const int ID = 1;

	StringPtr data1;//without anonymised src_id

	StringPtr data2;//anonymised src_id

	void recycle();
	~WrappedMessage();
};


class ThreadHandle;
class Connection;
class Node;

typedef int (*web_request_handler)(Request *req);
typedef struct ReaderTask{
	stCoRoutine_t* co_routine;
	int fd;
	ThreadHandle* thread_handle;
} ReaderTask;

typedef struct WriterTask{
	stCoRoutine_t* co_routine;
	Connection* conn;
	ThreadHandle* thread_handle;
} WriterTask;


typedef struct NewConnectionCreatorTask{
	stCoRoutine_t* co_routine;
	shared_ptr<Node> node;
	shared_ptr<WrappedMessage> message;
	ThreadHandle * thread_handle;
}NewConnectionCreatorTask;



typedef struct HttpsRequestTask{
	stCoRoutine_t* co_routine;
	string *url;
	map<string, string> *query_params;
	map<string, string> *headers;
	ThreadHandle * thread_handle;
}WebRequestTask;



class MessageTransitToThread{
public:
	static const int ID = 3;
	string dest_id;
	shared_ptr<WrappedMessage> msg = nullptr;
	size_t fd;
	void init(const string &dest_id , shared_ptr<WrappedMessage> &message , size_t fd);
	void recycle();
	~MessageTransitToThread();
};

class ThreadHandle{
public:
	tbb::concurrent_queue<shared_ptr<MessageTransitToThread>> messages_queue; // deal with messages , example: message received
	tbb::concurrent_queue<int> new_sockets; // newly accepted sockets , handle then in the thread

	queue<ReaderTask*> reader_tasks_pool;
	queue<WriterTask*> writer_tasks_pool;
	queue<NewConnectionCreatorTask*> connection_creator_tasks_pool;
	queue<HttpsRequestTask*> web_request_task_pool;

	pthread_t *thread_ref;
	map<int, Connection*> connections_ws;


	stShareStack_t* co_share_stack;
	ThreadHandle();
};

/*
 * from client or server it's just another line between nodes
 */




class Connection{
public:
	static const int ID = 2;

	queue<shared_ptr<WrappedMessage> , boost::container::deque<shared_ptr<WrappedMessage>>> pending_queue;// to keep track of how many greenlets are waiting on semaphore to send
	queue<pair<size_t , shared_ptr<WrappedMessage>> , boost::container::deque<pair<size_t , shared_ptr<WrappedMessage>>> > msg_assumed_sent; // queue for older sent messages in case of reset we try to retransmit
    int fd = -1;
    string from_node_id; //this objects owner is the node_id
    string to_node_id;
    ThreadHandle * thread_handle = NULL;

    ssize_t last_bytes_read = 0;
    string connection_id;
    bool is_external_node = true;
    long int last_msg_recv_timestamp = 0;
    long int last_msg_sent_timestamp = 0;
    long int created_timestamp = 0;

    long int ping_interval = 4*60*1000;//5 minutes , send a ping

    uint8_t client_type = 0;  // 1- raw data publisher  , 2- raw data subscriber

    uint8_t data_io_type = 0;

    bool should_save_resending_buffer;

    WriterTask *active_writer = NULL;

    SSL * ssl = NULL;
    bool is_ssl = false;

    Connection(): from_node_id(), to_node_id(), connection_id() , ssl(NULL){};

    ~Connection();
    void recycle();
    void init(SSL* ssl, string &to_node_id , ThreadHandle *thread_handle, bool is_ssl);
};

/*
 * just a point on graph , could indicate server or client
 */
class Node{
	public:
		string cluster_id;
		string node_id;
		string addr;
		string  addr_internal;
		string  port;
		long int created_timestamp = 0;
		long int cached_timestamp = 0;

		long int last_message_received_timestamp=0;

		bool is_ssl = false;
		bool is_internal = false;
		long _msg_recieved_counter = 0;
		//keep the copy on the to node only
		int _delta_connections = 0;
		int _update_on_num_connections_change = 100;

	 	string is_connected_to_node;

		Node():node_id(), addr(), addr_internal(), port(),created_timestamp(0L), cached_timestamp(0L) {};

};

class Session{
public:
	string session_id;
	string node_id;
	string description;
	int session_type;
	string session_game_master_node_id;
	long int created_timestamp;
	long int cached_timestamp;
	int who_can_add_session_nodes;
	bool allow_anonymous;
	int notify_only_last_few_users;
	int usage_type;
};

class SessionNode{
	public:
	string session_id;
	string node_id;
	bool is_anonymous = false;
	string anonymous_node_id;
	long int created_timestamp = 0;
};

class SessionNodesList:public map<string, SessionNode>{
public:
	long int cached_timestamp = 0;
};

class NodeSeq{
public:
	string node_id;
	long int seq;
	long int last_updated;
};



/*
 * Few function declarations
 */

Connection *track_new_connection(SSL* ssl,  shared_ptr<Node> &to_node, ThreadHandle *thread_handle, bool is_ssl);
void untrack_connection(Connection *conn, bool resend_last_messages, ThreadHandle *thread_handle);
void handle_connection(Connection *conn, ThreadHandle *thread_handle);

bool update_intermediate_node(const string &dest_id, const string &intermediate_node_id);


void on_message(shared_ptr<WrappedMessage> &msg, Connection* from_conn, int message_sending_flags, vector<string> *unable_to_send = NULL);

void close_ssl(SSL *ssl , int fd);

/*
 * utility functions
 */

void load_message_from_json(json &j, shared_ptr<WrappedMessage>& message);
void create_json_from_message(shared_ptr<WrappedMessage>& message , json &j);

string load_config_response_to_json(allspark::ConfigResponse &response);
void load_config_message_from(string *payload, allspark::ConfigRequest &config_request);

/*
 * extern constants
 */
extern shared_ptr<Node> current_node;


extern const int SESSION_TYPE_GAME;
extern const int SESSION_TYPE_NORMAL;



extern const int CLIENT_CONFIG_REQUEST;
extern const int CLIENT_CONFIG_RESPONSE;
extern const int USER_OFFLINE_RESPONSE;
extern const int SESSION_UNAUTHORIZED;
extern const int NEW_NODE_JOINED_SESSION;
extern const int NODE_UNJOINED_SESSION;
extern const int NODE_REVEAL_ANONYMITY;

extern const int SESSION_TYPE_GAME;
extern const int SESSION_TYPE_NORMAL;


typedef ConcurrentLRUCache<string, pair<ssize_t, string>> IntermediateHopsCache;

#endif /* SERVER_H_ */
