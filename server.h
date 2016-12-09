/*
 * server.h
 *
 *  Created on: Dec 4, 2016
 *      Author: abhinav
 */

#ifndef SERVER_H_
#define SERVER_H_


#include "tbb/concurrent_queue.h"
#include<map>


using std::map;
using std::string;
using tbb::concurrent_queue;
using tbb::concurrent_hash_map;
using folly::fbvector;


typedef void (*web_request_handler)(int, map<string, fbvector<string>>&, map<string, string>&);


string server_secret_key = "abhinav_reddy_1234";


class ThreadHandle{
	tbb::concurrent_queue<stCoRoutine_t*> co_wake_up_queue; // wake up these co routines , because they are waiting on something we triggered
	tbb::concurrent_queue<Message*> messages_queue; // deal with messages , example: message received
	tbb::concurrent_queue<int> new_sockets; // newly accepted sockets , handle then in the thread

	pthread_t *thread_ref;
};


class Message{
    char * dest_id;
    char *dest_client_id;
    char *dest_session_id;
    char *src_client_id;
    char *src_id;
    int type;
    char *payload;
    char *payload1;
    char *payload2;
    char *id;
    bool is_ack_required;
    bool anonymize_src_id;
    long timestamp;
};

class Connection{
	/*
	 * from client or server it's just another line between nodes
	 */
	concurrent_queue<Message*> queue;// to keep track of how many greenlets are waiting on semaphore to send
	concurrent_queue<Message*> msg_assumed_sent; // queue for older sent messages in case of reset we try to retransmit
    int ws = -1;
    string from_node_id; //this objects owner is the node_id
    string to_node_id;
    bool is_stale;
    string connection_id;
    bool is_external_node;
    long last_msg_recv_timestamp;
    long last_msg_sent_timestamp;
};

/*
 * just a point on graph , could indicate server or client
 */
class Node{
	public:
		char *cluster_id;
		char *node_id;
		char *addr;
		char * addr_internal;
		char * port;
		bool ssl_enabled;
		long _msg_recieved_counter = 0;
		//keep the copy on the to node only
		concurrent_hash_map<string, fbvector<Connection*>> connections;// node_id -> list of connection to it
		concurrent_hash_map<string, Connection*> connections_ws; // socket_fd -> websocket connection
		int _delta_connections = 0;
		int _update_on_num_connections_change = 100;
		int intermediate_hops;
};






#endif /* SERVER_H_ */
