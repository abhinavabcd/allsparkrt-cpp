/*
 * mysql_db.cpp
 *
 *  Created on: Nov 27, 2016
 *      Author: abhinav
 */


#include<iostream>
#include<stdlib.h>
#include<map>

using namespace std;




template<class T> class AutoScaleConnectionPool{
public:
	int current_size;
	int needed_size;
	T* get(){
	};
	void put_back(T* conn){
	};
	T* create(){};
};



/*
 * select a connection from pool and release back immediately after the query
 * if no connection available in pool , wake up the db thread that is sleeping to resize the pool,
 * after opening the connection send a signal back to the co routine thread to wake the coroutine
 * after the connection is opened.
 *
 */
class Db{
	public:
		Db(string addr, string port, string db_name, string username, string password, int poolsize=-1);
		int get_node_by_id(char * node_id);
		int set_node_health(char *node_id, int health);
		int get_connection_node();//called for a client who needs a client to connect
		int update_gcm_key(char *node_id, char * gcm_id);
		int get_node_with_connection_to(char *node_id);
		int check_and_add_new_connection(char *connection_id, char *node_id1, char *node_id2);
		int is_server_node(char * node_id);
		int add_connection(char * node_id1 , char *node_id2);
		int remove_connection(char * connection_id);
		int get_node_ids_by_client_id(char* client_id);
		int get_node_ids_for_group(char *group_id);
		int get_node_ids_for_session(char *session_id);
		int create_node(char *client_id , char *addr , char *addr_internal, int port, bool is_server=false);
		int node_config_exists(char *addr, char *port);
		int clear_connections_to_node_from_db(char * node_id);
		int create_session(char *node_id, char *session_id, int session_type=0, char* session_game_master_node_id=NULL, int notify_only_last_few_users=-1, bool anyone_can_join=false);
		int get_session_by_id(char *session_id);
		int join_session(char * session_id , char *node_id, bool is_anonymous=false, bool update_in_db=true, char *anonymous_node_id=NULL);
		int unjoin_session(char *session_id, char *node_id , bool update_in_db=true);
		int reveal_anonymity(char * session_id, char *node_id, bool update_in_db=true);
		int remove_client_nodes(char * client_id);
		int add_pending_messages(char * node_id, char *message_type, char *message_json, char * current_timestamp=NULL);
		int fetch_inbox_messages(char *node_id , int from_seq=-1, int to_seq = -1,  int timea=-1 , int timeb=-1);
		int get_seq(char * node_id, bool update);
};

