/*
 * mysql_db.h
 *
 *  Created on: Dec 12, 2016
 *      Author: abhinav
 */

#ifndef MYSQL_DB_H_
#define MYSQL_DB_H_

#include "mysql_connection.h"

#include "cppconn/statement.h"
#include "cppconn/resultset.h"
#include "cppconn/prepared_statement.h"

#include "tbb/concurrent_queue.h"
#include <boost/shared_ptr.hpp>
#include <boost/move/unique_ptr.hpp>
#include "wire_messages/wire_transfers.pb.h"
#include <iostream>
#include <stdlib.h>
#include <map>
#include "server.h"
#include "utilities/concurrent_lru_cache.h"
#include <sys/time.h>



using boost::movelib::unique_ptr;
using boost::shared_ptr;
using allspark::InboxMessage;
using tbb::concurrent_queue;
using std::string;
using std::pair;
class DatabaseWrapper;


typedef ConcurrentLRUCache<string, shared_ptr<Node>> NodeCache;
typedef ConcurrentLRUCache<string, shared_ptr<Session>> SessionCache;

typedef ConcurrentLRUCache<string, shared_ptr<SessionNodesList>> SessionNodeCache;
typedef ConcurrentLRUCache<string, shared_ptr<NodeSeq>> NodeSeqCache;


class MysqlDbConnection{
  public:
	bool reconnect_state = true;
	sql::Driver *driver = NULL;
	sql::Connection *mysql_conn = NULL;
	sql::Statement *stmt = NULL;
	sql::ResultSet *res =NULL;
	sql::PreparedStatement *pstmt = NULL;
	void reset();
	MysqlDbConnection();
	static MysqlDbConnection *getInstance(const string &addr ,  const string &db_name, const string &user_name, const string &password);
};

class MysqlDbInstance{
	public:
		int instance_id;

		string addr;
		string db_name;
		string db_user_name;
		string db_password;
		concurrent_queue<MysqlDbConnection*> connection_pool;

		MysqlDbInstance(int instance_id, const string &addr, const string &db_name, const string &db_user_name, const string &db_password);

};

class EsInstance{
	public:
		int instance_id;
		string addr;
		string db_name;
		string db_user_name;
		string db_password;
		concurrent_queue<MysqlDbConnection*> connection_pool;

		EsInstance(const int instance_id, const string &addr, const string &db_name, const string &db_user_name, const string &db_password);

};


class DatabaseWrapper{

	concurrent_hash_map<int, MysqlDbInstance*> db_instances_set;
	concurrent_hash_map<int, EsInstance*> elastic_search_instances_set;

	MysqlDbConnection* _get_db_connection(const MysqlDbInstance *instance);
	void _release_db_connection(const MysqlDbInstance *db_instance, MysqlDbConnection *conn);
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> get_db_pooled_connection(int instance_id);

   public :
	DatabaseWrapper();

	void init();

	unique_ptr<NodeCache> node_cache = unique_ptr<NodeCache>(new NodeCache(100000));
	unique_ptr<SessionCache>  session_cache = unique_ptr<SessionCache>(new SessionCache(100000));
	unique_ptr<SessionNodeCache> session_node_cache = unique_ptr<SessionNodeCache>(new SessionNodeCache(100000));
	unique_ptr<NodeSeqCache> node_seq_cache = unique_ptr<NodeSeqCache>(new NodeSeqCache(100000));

	void close_all_connections();
	void clear_all_caches();


	long int get_node_last_ping_received(string &node_id);
	bool set_node_last_ping_received(string &node_id, long int now);
	shared_ptr<Node> get_node_by_id(const string &node_id ,  bool strict_check , bool force_refresh_from_db);
	bool get_node_from_result_set(sql::ResultSet *res , shared_ptr<Node> &node);
	unique_ptr<Node> get_connection_node();
	int update_push_key(const string &node_id, const string &gcm_key, int type);
	const string get_push_key(const string &node_id, int &type);

//	shared_ptr<Node> get_node_with_connection_to(const string &node_id);
//	bool add_new_connection(Connection *connection);
//	int remove_connection(string &connection_id);
//

	int remove_node(string &node_id);

	bool set_is_connected_to_node(string &node_id, string *to_node_id);

	shared_ptr<Node> create_node( const string &addr ,const string &port, bool is_internal=false, bool is_ssl=false);
	shared_ptr<Node> node_config_exists(string &addr, string &port);

	bool is_node_in_session(const string &session_id , const string &node_id);
	shared_ptr<Session> create_session(string &node_id, string *session_id, int session_type, string *session_game_master_node_id, int notify_only_last_few_users, int who_can_join_new_nodes, bool join_anonymous, int usage_type, bool allow_anonymous, string *description);
	shared_ptr<Session> get_session_by_id(const string &session_id);
	bool get_session_from_result_set(sql::ResultSet *res , shared_ptr<Session> &session);
	shared_ptr<SessionNodesList> get_node_ids_for_session(const string &session_id, bool force_load_from_db=false);
	bool _get_session_nodes_from_result_set(sql::ResultSet *res, shared_ptr<SessionNodesList>& session_nodes);
	int join_session(const string &session_id , string *node_id, string *to_join_session_or_node_id, bool is_anonymous=false, bool update_in_db=true, string *anonymous_node_id=NULL);
	bool unjoin_session(const string &session_id, const string &node_id , bool update_in_db=true);
	bool reveal_anonymity(const string &session_id, const string &node_id, bool update_in_db=true);


	bool add_message_to_inbox(string *node_id, Message *msg);
	int add_pending_messages(string *inbox_id, string *from_id, int message_type, char * payload, size_t payload_len, long int current_timestamp=-1);
	bool inbox_sorting_comparator (InboxMessage *i,InboxMessage *j);
	unique_ptr<vector<InboxMessage*>> fetch_inbox_messages(string *node_id , bool &more, long int from_seq=-1, long int to_seq = -1,  long int timea=-1 , long int timeb=-1 , boost::function<InboxMessage*()> *message_allocator = NULL);
	long int get_seq(string *node_id, bool update);
	bool get_node_seq_from_db(string *node_id, shared_ptr<NodeSeq> &node_seq);

	int get_sessions_by_node_id(const string &node_id, int session_type, int usage_type, int &from, boost::function<allspark::SessionData*()> *inbox_message_allocator);
};

#endif /* MYSQL_DB_H_ */
