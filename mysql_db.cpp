/*
 * mysql_db.cpp
 *
 *  Created on: Nov 27, 2016
 *      Author: abhinav
 */


#include <sstream>
#include <iostream>
#include <boost/make_shared.hpp>

#include "boost/bind.hpp"
#include "boost/function.hpp"
#include "boost/move/unique_ptr.hpp"
#include "boost/move/make_unique.hpp"
#include "boost/scoped_ptr.hpp"
#include "boost/exception/diagnostic_information.hpp"


#include "mysql_driver.h"
#include "spdlog/spdlog.h"



#include "mysql_db.h"
#include "config.h"
#include "utils.h"
#include "utilities/conversion_utils/conversion_utils.h"


#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <netdb.h>
#include "constants.h"

/*
 * select a connection from pool and release back immediately after the query
 * if no connection available in pool , wake up the db thread that is sleeping to resize the pool,
 * after opening the connection send a signal back to the co routine thread to wake the coroutine
 * after the connection is opened.
 *
 * Db intance is a deployment, which may contain multiple shards, read replicas etc, all this is one instance.
 *
 */




using boost::scoped_ptr;
using boost::movelib::unique_ptr;
using boost::movelib::make_unique;
using tbb::concurrent_queue;
using boost::function;
using boost::shared_ptr;
using std::stringstream;
using std::cout;
using std::endl;

using allspark::Message;
extern shared_ptr<Node> current_node;
extern std::shared_ptr<spdlog::logger> server_logger;

/*
 * One connection per requestor , nothing shared between multiple requestors , request and release model
 */

MysqlDbConnection::MysqlDbConnection() :reconnect_state (true), driver(NULL), mysql_conn (NULL), stmt(NULL), res(NULL), pstmt(NULL){};
MysqlDbInstance::MysqlDbInstance(int instance_id, const string &addr, const string &db_name, const string &db_user_name, const string &db_password) : instance_id(instance_id), addr(addr), db_name(db_name) , db_user_name(db_user_name), db_password(db_password) {};


static MysqlDbConnection* MysqlDbConnection::getInstance(const string &addr,  const string &db_name, const string &user_name, const string &password){
	MysqlDbConnection *ret = new MysqlDbConnection();
	try{
		ret->driver = get_driver_instance();
		ret->mysql_conn = ret->driver->connect(addr, user_name, password);
		ret->mysql_conn->setClientOption("OPT_RECONNECT", &ret->reconnect_state);
		ret->mysql_conn->setSchema(db_name);
	}
	catch (sql::SQLException &ex){
		ret->reset();
		if(ret->mysql_conn){
			ret->mysql_conn->close();
			delete ret->mysql_conn;
		}
		delete ret;
		return NULL;
	}
    return ret;
}

void MysqlDbConnection::reset(){
	try{
		if(stmt!=NULL){
			stmt->close();
			delete stmt;
			stmt  = NULL;
		}
	}
	catch(sql::SQLException & e){
		server_logger->error(e.what());
	}


	try{
		if(pstmt!=NULL){
			pstmt->close();
			delete pstmt;
			pstmt = NULL;
		}
	}
	catch(sql::SQLException & e){
		server_logger->error(e.what());
	}


	try{
		if(res!=NULL){
			res->close();
			delete res;
			res = NULL;
		}
	}
	catch(sql::SQLException & e){
		server_logger->error(e.what());
	}
}


DatabaseWrapper::DatabaseWrapper(){
	//create a dfault db instance
}


void DatabaseWrapper::init(){
	concurrent_hash_map<int, MysqlDbInstance*>::accessor db_instance_accessor;
	db_instances_set.insert(db_instance_accessor, server_config.default_db_instance_id);
	db_instance_accessor->second = new MysqlDbInstance(server_config.default_db_instance_id, server_config.default_db_host, server_config.default_db_name, server_config.default_db_username , server_config.default_db_password);
}

bool DatabaseWrapper::set_node_last_ping_received(string &node_id, long int now){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	shared_ptr<Node> node = get_node_by_id(node_id, false, false);
	long int  diff = -1;
	if(node){
		diff = now - node->last_message_received_timestamp;
		node->last_message_received_timestamp = now;
	}
	if(diff!=-1 && diff < 10*60*1000){//10 min
		return true;
	}
	db->pstmt = db->mysql_conn->prepareStatement("update nodes set last_ping_received= ? where node_id = ?");
	db->pstmt->setInt64(1, now);
	db->pstmt->setString(2, node_id);
	return db->pstmt->executeUpdate();
}

long int DatabaseWrapper::get_node_last_ping_received(string &node_id){
	shared_ptr<Node> node = get_node_by_id(node_id, true, false);
	if(node){
		return node->last_message_received_timestamp;
	}
	return 0;
}


shared_ptr<Node> DatabaseWrapper::get_node_by_id(const string &node_id ,  bool strict_check , bool force_refresh_from_db){
	if(node_id.empty()){
		return nullptr;
	}
	NodeCache::ConstAccessor ac;
	if ( !force_refresh_from_db ){
        if(node_cache->find(ac, node_id)) {
        	return *ac;
        }
	}
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from nodes where node_id = ?");
	db->pstmt->setString(1, node_id);
	db->res = db->pstmt->executeQuery();

	shared_ptr<Node> node = boost::make_shared<Node>();
	if(!get_node_from_result_set(db->res, node)){
		if(strict_check){
			return nullptr;
		}
		db->reset();//reset prepared statements
		db->pstmt = db->mysql_conn->prepareStatement("insert into nodes (node_id , is_internal) values(? , ?)");
		db->pstmt->setString(1, node_id);
		db->pstmt->setBoolean(2, false);
		if(!db->pstmt->executeUpdate()){
			return nullptr;
		}

		node->node_id = node_id;
		node->is_internal = false; //external
	}

	//replace old one even if it exists
	node_cache->insert(node_id, node);
	node->cached_timestamp = get_time_in_ms();
	return node;
}


bool DatabaseWrapper::get_node_from_result_set(sql::ResultSet *res , shared_ptr<Node> &node){
	while(res->next()){
		node->node_id = res->getString(COLUMN_NODE_ID);
		node->addr = res->getString(COLUMN_ADDR);
		node->addr_internal = res->getString(COLUMN_ADDR_INTERNAL);
		node->port = res->getString(COLUMN_PORT);
		node->is_ssl = res->getBoolean(COLUMN_NODE_IS_SSL);
		node->is_internal = res->getBoolean(COLUMN_IS_INTERNAL);
		node->created_timestamp = res->getInt64(COLUMN_NODE_CREATED_TIMESTAMP);
		node->last_message_received_timestamp = res->getInt64(COLUMN_NODE_LAST_MESSAGE_RECEIVED_TIMESTAMP);
		node->is_connected_to_node = res->getString(COLUMN_NODE_IS_CONNECTED_TO);
		return true;
	}
	return false;
}

unique_ptr<Node> DatabaseWrapper::get_connection_node(){//called for a client who needs a client to connect
	return NULL;
}

int DatabaseWrapper::update_push_key(const string &node_id, const string &gcm_key, int type){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("INSERT INTO node_push_keys (node_id, type, push_key) VALUES(?, ?, ?) ON DUPLICATE KEY UPDATE type=?, push_key=?");
	db->pstmt->setString(1, node_id);
	db->pstmt->setInt(2, type);
	db->pstmt->setString(3, gcm_key);

	db->pstmt->setInt(4, type);
	db->pstmt->setString(5, gcm_key);

	return db->pstmt->executeUpdate();
}

const string DatabaseWrapper::get_push_key(const string &node_id , int &type){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from node_push_keys where node_id = ? ");
	db->pstmt->setString(1, node_id);
	db->res = db->pstmt->executeQuery();
	while(db->res->next()){
		type = db->res->getInt(COLUMN_NODE_PUSH_KEYS_TYPE);
		return db->res->getString(COLUMN_NODE_PUSH_KEYS_PUSH_KEY);
	}
	return "";
}




bool DatabaseWrapper::set_is_connected_to_node(string &node_id, string *to_node_id){
	shared_ptr<Node> node = get_node_by_id(node_id, true , false);
	if(!node){
		return false;
	}

	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("update nodes set is_connected_to_node=? where node_id = ? ");
	if(to_node_id==NULL){
		node->is_connected_to_node.clear();
		db->pstmt->setNull(1, NULL);
	}
	else{
		db->pstmt->setString(1, *to_node_id);
		node->is_connected_to_node = *to_node_id;
	}
	db->pstmt->setString(2, node->node_id);
	bool ret = db->pstmt->executeUpdate();
	return ret;
}

//bool get_connection_from_result_set(sql::ResultSet *res , Connection &connection){
//	while(res->next()){
//		connection.connection_id = res->getString(COLUMN_CONNECTION_ID);
//		connection.from_node_id = res->getString(COLUMN_FROM_NODE_ID);
//		connection.to_node_id = res->getString(COLUMN_TO_NODE_ID);
//		return true;
//	}
//	return false;
//}
//
//shared_ptr<Node> DatabaseWrapper::get_node_with_connection_to(const string &node_id){
//	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
//	db->pstmt = db->mysql_conn->prepareStatement("select * from connections where to_node_id = ? or from_node_id = ?");
//	db->pstmt->setString(1, node_id);
//	db->pstmt->setString(2, node_id);
//	db->res = db->pstmt->executeQuery();
//	Connection c;
//	if(get_connection_from_result_set(db->res, c)){
//
//		if(c.from_node_id==node_id){
//			return get_node_by_id(c.to_node_id, true, false);
//		}
//		else{
//			return get_node_by_id(c.from_node_id, true, false);
//		}
//	}
//	return nullptr;
//}
//
//bool DatabaseWrapper::add_new_connection(Connection *connection){
//	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
//	char connection_id_ptr[CONNECTION_STR_LEN];
//	int retry = 0;
//	while(retry++<10){
//		gen_random(connection_id_ptr, CONNECTION_STR_LEN);
//		sql::SQLString connection_id(connection_id_ptr, CONNECTION_STR_LEN);
//		db->pstmt = db->mysql_conn->prepareStatement("insert into connections(connection_id, from_node_id, to_node_id, created_timestamp) values(? , ? , ?, ?)");
//		db->pstmt->setString(1, connection_id);
//		db->pstmt->setString(2, *(connection->from_node_id.empty()?&current_node->node_id:&connection->from_node_id));
//		db->pstmt->setString(3, connection->to_node_id);
//		db->pstmt->setInt64(4, connection->created_timestamp);
//
//		if(db->pstmt->executeUpdate()){
//			connection->connection_id = connection_id;
//			return true;
//		}
//		db->reset();
//	}
//	return false;
//}
//
//int DatabaseWrapper::remove_connection(string &connection_id){
//	int retry_count = 20;
//	while(retry_count>0){
//		try{
//			unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
//			db->pstmt = db->mysql_conn->prepareStatement("delete from connections where connection_id=?");
//			db->pstmt->setString(1, connection_id);
//			return db->pstmt->executeUpdate();
//		}
//		catch(sql::SQLException &ex){
//			server_logger->error("unable to remove connection ..retrying {} ",connection_id);
//		}
//		retry_count--;
//	}
//	return false;
//}

int DatabaseWrapper::remove_node(string &node_id){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("delete from nodes where node_id=?");
	db->pstmt->setString(1, node_id);
	return db->pstmt->executeUpdate();
}


shared_ptr<Node> DatabaseWrapper::create_node( const string &addr ,const string &port, bool is_internal=false, bool is_ssl=false){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	char _node_id[35];
	int retry_count = 0;
	while(retry_count++<10){
		long int now  = get_time_in_ms();
		int l = gen_random(_node_id, 0);
		string node_id(_node_id, l);
		db->pstmt = db->mysql_conn->prepareStatement("insert into nodes(node_id, addr, port, is_internal, created_timestamp, is_ssl) values(? , ? , ? , ? , ? , ?)");
		db->pstmt->setString(1, node_id);
		db->pstmt->setString(2, addr);
		db->pstmt->setString(3, port);
		db->pstmt->setBoolean(4, is_internal);
		db->pstmt->setInt64(5, now);
		db->pstmt->setBoolean(6, is_ssl);
		if(db->pstmt->executeUpdate()){
			shared_ptr<Node> node = boost::make_shared<Node>();
			node->node_id = node_id;
			node->addr = addr;
			node->port = port;
			node->is_internal = is_internal;
			node->created_timestamp = now;
			node_cache->insert(node->node_id, node);
			node->cached_timestamp = now;
			node->is_ssl = is_ssl;
			return node;
		}
		db->reset();
	}
	return nullptr;
}

shared_ptr<Node> DatabaseWrapper::node_config_exists(string &addr, string &port){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from nodes where addr = ? AND port = ?");
	db->pstmt->setString(1, addr);
	db->pstmt->setString(2, port);
	db->res = db->pstmt->executeQuery();
	while(db->res->next()){
		shared_ptr<Node> node = boost::make_shared<Node>();
		node->node_id = db->res->getString(COLUMN_NODE_ID);
		node->addr = addr;
		node->port = port;
		node->is_internal = db->res->getBoolean(COLUMN_IS_INTERNAL);
		node->is_ssl = db->res->getBoolean(COLUMN_NODE_IS_SSL);

		return node;
	}
	return nullptr;
}

/*
 * session function start , get session , create session
 *
 * usage_type = USAGE_TYPE_PLAIN_SESSION =0 , USAGE_TYPE_TEMPLATE =1
 */
shared_ptr<Session> DatabaseWrapper::create_session(string &node_id, string *session_id, int session_type, string *session_game_master_node_id, int notify_only_last_few_users, int who_can_add_session_nodes, bool join_anonymous, int usage_type, bool allow_anonymous, string * description){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	if(notify_only_last_few_users!=-1){
		notify_only_last_few_users = std::max(1000 , notify_only_last_few_users);
	}
	else{
		notify_only_last_few_users = 1000;
	}
	long int now = get_time_in_ms();


	string _session_id;
	if(session_id==NULL){
		char temp[35];
		int l = gen_random(temp, 0);
		_session_id.append(temp , l);
		session_id = &_session_id;
	}

	db->pstmt = db->mysql_conn->prepareStatement("insert into sessions(session_id, node_id,  session_type, session_game_master_node_id , notify_only_last_few_users, who_can_add_session_nodes, created_timestamp, usage_type , allow_anonymous, description) values(? , ? , ? , ? , ? , ?, ?, ? , ?, ?)");
	db->pstmt->setString(1,  *session_id);
	db->pstmt->setString(2, node_id);
	db->pstmt->setInt(3, session_type);
	if(session_type == SESSION_BROADCAST_TYPE_GAME && session_game_master_node_id){
		db->pstmt->setString(4, *session_game_master_node_id);
	}
	else{
		db->pstmt->setNull(4, NULL);
	}
	db->pstmt->setInt(5, notify_only_last_few_users);
	db->pstmt->setInt(6, who_can_add_session_nodes);
	db->pstmt->setInt64(7,  now);
	db->pstmt->setInt(8,  usage_type);
	db->pstmt->setBoolean(9,  allow_anonymous);
	if(description){
		db->pstmt->setString(10,  *description);
	}
	else{
		db->pstmt->setNull(10 , NULL);
	}


	if(db->pstmt->executeUpdate()){
		shared_ptr<Session> session= boost::make_shared<Session>();
		session->session_id = *session_id;
		session->node_id = node_id;
		session->created_timestamp = now;
		session->session_type = session_type;

		if(session->session_type == SESSION_BROADCAST_TYPE_GAME){
			session->session_game_master_node_id = std::move(*session_game_master_node_id);
		}
		session->notify_only_last_few_users = notify_only_last_few_users;
		session->who_can_add_session_nodes = who_can_add_session_nodes;
		session->allow_anonymous = allow_anonymous;
		if(description)
			session->description = std::move(*description);
		session->usage_type = usage_type;
		session_cache->insert(*session_id, session);
		session->cached_timestamp = now;

		if(session->session_type!=SESSION_BROADCAST_TYPE_GAME){ //game type node , will not join the sessions , it's kinda master node
			join_session(session->session_id, NULL , &session->node_id,  join_anonymous, true, NULL);
		}
		return session;
	}

	return nullptr;

}

shared_ptr<Session> DatabaseWrapper::get_session_by_id(const string &session_id){
	if(session_id.empty()){
		return nullptr;
	}
	SessionCache::ConstAccessor ac;
	if (session_cache->find(ac, session_id)) {
		return *ac;
	}
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from sessions where session_id = ?");
	db->pstmt->setString(1, session_id);
	db->res = db->pstmt->executeQuery();

	if(db->res->next()){
		shared_ptr<Session> session= boost::make_shared<Session>();
		session->session_id = db->res->getString(COLUMN_SESSIONS_SESSION_ID);
		session->node_id = db->res->getString(COLUMN_SESSIONS_NODE_ID);

		session->created_timestamp = db->res->getInt64(COLUMN_SESSIONS_CREATED_AT);
		session->session_type = db->res->getInt(COLUMN_SESSIOS_SESSION_TYPE);
		session->session_game_master_node_id = db->res->getString(COLUMN_SESSIONS_SESSION_MASTER_NODE_ID);
		session->notify_only_last_few_users = db->res->getInt(COLUMN_SESSIONS_LAST_FEW_USERS);
		session->who_can_add_session_nodes = db->res->getBoolean(COLUMN_SESSIONS_WHO_CAN_JOIN_NEW_NODES);
		session->usage_type = db->res->getBoolean(COLUMN_SESSIONS_USAGE_TYPE);
		session->allow_anonymous = db->res->getBoolean(COLUMN_SESSIONS_ALLOW_ANONYMOUS);
		session->description = db->res->getString(COLUMN_SESSIONS_DESCRIPTION);


		session_cache->insert(session_id, session);
		session->cached_timestamp = get_time_in_ms();

		return session;
	}
	return nullptr;
}



shared_ptr<SessionNodesList> DatabaseWrapper::get_node_ids_for_session(const string &session_id, bool force_load_from_db=false){
	SessionNodeCache::ConstAccessor ac;
	shared_ptr<SessionNodesList> ret = nullptr;
	if (session_node_cache->find(ac, session_id)) {
		if(!force_load_from_db){
			return *ac;
		}
		else{
			ret = *ac; //move constructor ?? TODO:: check
		}
	}
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	long int max_timestamp = 0;
	if(ret!=nullptr){
		//there are nodes already in cache, you should probably select new nodes from db that have an higher timestamp
		for(map<string, SessionNode>::iterator it = ret->begin();it!=ret->end();it++){
			max_timestamp = MAX(it->second.created_timestamp, max_timestamp);
		}
	}
	ac.m_hashAccessor.release();


	db->pstmt = db->mysql_conn->prepareStatement("select * from session_nodes where session_id=? and created_timestamp >= ? order by created_timestamp DESC");
	db->pstmt->setString(1, session_id);
	db->pstmt->setInt64(2, max_timestamp);
	db->res=  db->pstmt->executeQuery();
	if(ret==nullptr){
		 ret  = boost::make_shared<SessionNodesList>();
	}
	if(_get_session_nodes_from_result_set(db->res, ret)){
		session_node_cache->insert(session_id, ret);
		ret->cached_timestamp = get_time_in_ms();
		return ret;
	}
	return nullptr;
}


bool DatabaseWrapper::_get_session_nodes_from_result_set(sql::ResultSet *res, shared_ptr<SessionNodesList>& session_nodes){
	bool has_nodes = false;
	while(res->next()){
		SessionNode session_node;
		session_node.node_id = res->getString(COLUMN_SESSION_NODES_NODE_ID);
		session_node.anonymous_node_id = res->getString(COLUMN_SESSION_NODES_ANONYMOUS_NODE_ID);
		session_node.is_anonymous = res->getBoolean(COLUMN_SESSION_NODES_IS_ANONYMOUS);
		//session_node.is_session = res->getBoolean(COLUMN_SESSION_NODES_IS_SESSION);
		(*session_nodes)[session_node.node_id] = std::move(session_node);
		has_nodes = true;
	}
	return has_nodes;
}

bool DatabaseWrapper::is_node_in_session(const string &session_id , const string &node_id){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from session_nodes where session_id_node_id = ?");
	db->pstmt->setString(1 , session_id+"_"+node_id);
	db->res = db->pstmt->executeQuery();
	if(db->res->next()){
		return true;
	}
	return false;
}

int DatabaseWrapper::join_session(const string &session_id , string *node_id, string *to_join_node_or_session_id, bool is_anonymous, bool update_in_db, string *anonymous_node_id){
	shared_ptr<Session> session = get_session_by_id(session_id);
	if(session == nullptr){
		//TODO: add log ,no such session ,  node_id, session_id
		return SESSION_JOINING_ERROR;
	}
	shared_ptr<SessionNodesList> session_nodes = get_node_ids_for_session(session_id);
//	if(session_nodes!=nullptr){
//		map<string, SessionNode>::iterator session_node_iterator = session_nodes->find(to_join_node_or_session_id);
//		if(session_node_iterator!=session_nodes->end()){
//			//already existent
//			return SESSION_ALREADY_JOINED; //session_node
//		}
//	}

	if(to_join_node_or_session_id==NULL){
		to_join_node_or_session_id = node_id;
	}
	if(is_node_in_session(session_id, *to_join_node_or_session_id)){
		return SESSION_ALREADY_JOINED; //session_node
	}

	if(session->who_can_add_session_nodes == SESSION_OWNER_ONLY_CAN_ADD){
		if(node_id!=NULL && node_id->compare(session->node_id)!=0 ){
			return SESSION_JOINING_ERROR;
		}
	}
	else if(session->who_can_add_session_nodes == SESSSION_NODES_CAN_ADD){
		if(node_id!=NULL && !is_node_in_session(session_id, *node_id)){
			return SESSION_JOINING_ERROR;
		}
	}

	SessionNode session_node;
	session_node.session_id = session_id;
	session_node.node_id = *to_join_node_or_session_id;
	if(is_anonymous && session->allow_anonymous){
		string anonymous_node_id_str;
		if(anonymous_node_id==NULL){
			char arr[35];
			int l = gen_random(arr, 0);
			anonymous_node_id_str = string(arr, l);
			anonymous_node_id = &anonymous_node_id_str;
		}

		session_node.anonymous_node_id = *anonymous_node_id;
		session_node.is_anonymous = true;
	}

	if (update_in_db) {
		unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
		db->pstmt = db->mysql_conn->prepareStatement("insert into session_nodes(session_id_node_id, session_id , node_id, anonymous_node_id, is_anonymous, timestamp) values( ?, ?,  ? , ? , ?, ? )");
		db->pstmt->setString(COLUMN_SESSION_NODES_SESSION_ID_NODE_ID, session_id+"_"+(*to_join_node_or_session_id));
		db->pstmt->setString(COLUMN_SESSION_NODES_SESSION_ID, session_id);
		db->pstmt->setString(COLUMN_SESSION_NODES_NODE_ID, *to_join_node_or_session_id);
		if(is_anonymous){
			db->pstmt->setString(COLUMN_SESSION_NODES_ANONYMOUS_NODE_ID, *anonymous_node_id);
		}
		else{
			db->pstmt->setNull(COLUMN_SESSION_NODES_ANONYMOUS_NODE_ID, NULL);

		}
		db->pstmt->setBoolean(COLUMN_SESSION_NODES_IS_ANONYMOUS, is_anonymous);
		db->pstmt->setInt64(COLUMN_SESSION_NODES_TIMESTAMP, get_time_in_ms());

		if(!db->pstmt->executeUpdate()){
			return SESSION_JOINING_ERROR;
		}
	}
	if(session_nodes!=nullptr){
		(*session_nodes)[*to_join_node_or_session_id] = session_node;
	}
	if(session->notify_only_last_few_users>0){
		//keep the size of sessions nodes limited
		//TODO:
	}
	return SESSION_JOINED;//copy the data structure
}

bool DatabaseWrapper::unjoin_session(const string &session_id, const string &node_id , bool update_in_db=true){
	bool is_updated = true;
	if(update_in_db){
		unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
		db->pstmt = db->mysql_conn->prepareStatement("delete from session_nodes where session_id_node_id = ?");
		db->pstmt->setString(1, session_id+"_"+node_id);
		is_updated = db->pstmt->executeUpdate()!=0;
	}
	if(is_updated){
		shared_ptr<map<string, SessionNode>> session_nodes = get_node_ids_for_session(session_id);
		if(session_nodes!=nullptr){
			session_nodes->erase(node_id);
		}
		return true;
	}
	return false;
}

bool DatabaseWrapper::reveal_anonymity(const string &session_id, const string &node_id, bool update_in_db=true){
	shared_ptr<map<string, SessionNode>> session_nodes = get_node_ids_for_session(session_id);
	auto session_node_iterator = session_nodes->find(node_id);
	bool is_updated = true;
	if(update_in_db){
		unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
		db->pstmt =db->mysql_conn->prepareStatement("update session_nodes set is_anonymous = ? where session_id_node_id = ?");
		db->pstmt->setBoolean(1, false);
		db->pstmt->setString(2, session_id+"_"+node_id);
		is_updated = db->pstmt->executeUpdate()!=0;
	}
	if(is_updated && session_node_iterator!=session_nodes->end()){
		session_node_iterator->second.is_anonymous = false;
		return true;
	}
	return false;

}


int DatabaseWrapper::get_sessions_by_node_id(const string &node_id, int session_type, int usage_type, int &from, boost::function<allspark::SessionData*()> *session_allocator){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select session_id from sessions where node_id = ? and session_type= ? and usage_type=? limit ?, 100");
	db->pstmt->setString(1, node_id);
	db->pstmt->setInt(2, SESSION_BROADCAST_TYPE_NORMAL);
	db->pstmt->setInt(3, USAGE_TYPE_PLAIN_SESSION);
	db->pstmt->setInt(4, from);
	int ret = 0;
	db->res = db->pstmt->executeQuery();
	while(db->res->next()){
		string session_id = db->res->getString("session_id");
		shared_ptr<Session> session = get_session_by_id(session_id);
		shared_ptr<SessionNodesList> session_nodes = get_node_ids_for_session(session_id, false);

		allspark::SessionData *session_data = (*session_allocator)();
		ret++;
		session_data->set_session_id(session->session_id);
		session_data->set_description(session->description);
		session_data->set_node_id(session->node_id);
		session_data->set_allow_anonymous(session->allow_anonymous);

		if(session_nodes){
			for(SessionNodesList::iterator it = session_nodes->begin(); it!=session_nodes->end();it++){
				allspark::SessionNodeData * session_node = session_data->add_session_nodes();
				if(it->second.is_anonymous){
					session_node->set_node_id(it->second.anonymous_node_id);
					session_node->set_is_anonymous(true);
				}
				else{
					session_node->set_node_id(it->second.node_id);
				}
			}
		}
	}
	return ret;
}



//session functions end





class DataBuf : public std::streambuf
{
public:
   DataBuf(char * d, size_t s) {
      setg(d, d, d + s);
   }
};


bool DatabaseWrapper::add_message_to_inbox(string *node_id , Message *msg){
	string inbox_id;
	if(node_id==NULL){
		node_id = msg->mutable_src_id();
	}
	if(!msg->dest_id().empty()){
		shared_ptr<Node> node = get_node_by_id(msg->dest_id(), true, false);
		if(node && !node->is_internal){
			if(msg->dest_id() < *node_id){
				inbox_id = msg->dest_id() + "_" + (*node_id ) ;
			}
			else{
				inbox_id =  (*node_id ) +"_"+ msg->dest_id() ;
			}
		}
	}
	else if(!msg->dest_session_id().empty()){
		shared_ptr<Session> session = get_session_by_id(msg->dest_session_id());
		if(!session || !is_node_in_session(session->session_id, *node_id)){
			return false;
		}
		inbox_id = session->session_id;
	}


	if(!inbox_id.empty()){
		string msg_str = msg->SerializeAsString();
		if(add_pending_messages(&inbox_id, node_id, msg->type(),   &msg_str[0] , msg_str.length(), get_time_in_ms())){
			return true;
		}
	}
	return false;
}

int DatabaseWrapper::add_pending_messages(string *inbox_id, string *from_id, int message_type, char * payload, size_t payload_len, long int current_timestamp=-1){
	long int seq = get_seq(inbox_id, true);
	if(current_timestamp==-1){
		current_timestamp  = get_time_in_ms();
	}
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("insert into user_inbox(node_id_seq, from_id , message_type, payload, timestamp) values(? , ?, ?, ? , ?)");
	stringstream ss;
	ss << *inbox_id << "_" << seq;
	db->pstmt->setString(1, ss.str());
	if(from_id!=NULL){
		db->pstmt->setString(2, *from_id);
	}
	else{
		db->pstmt->setNull(2,NULL);
	}
	db->pstmt->setInt(3, message_type);

//	DataBuf buffer(payload, payload_len);
//	std::istream stream(&buffer);

	sql::SQLString _payload(payload, payload_len);
	db->pstmt->setString(4, _payload);
	db->pstmt->setInt64(5, current_timestamp);
	return db->pstmt->executeUpdate();
}

bool DatabaseWrapper::inbox_sorting_comparator (InboxMessage *i,InboxMessage *j) { return (i->timestamp() < j->timestamp()); }
unique_ptr<vector<InboxMessage*>> DatabaseWrapper::fetch_inbox_messages(string *node_id , bool &more, long int from_seq=-1, long int to_seq = -1,  long int timea=-1 , long int timeb=-1,  boost::function<InboxMessage*()> *inbox_message_allocator = NULL){
	if(to_seq==-1){
		to_seq = get_seq(node_id, false);
	}
	if(from_seq==-1 || (to_seq - from_seq)>20){
		from_seq = std::max(0L , to_seq - 20);
	}
	if(timea==-1){
		timea=0;
	}
	if(timeb<timea){
		timeb= get_time_in_ms();
	}

	unique_ptr<vector<InboxMessage*>> inbox_messages  = boost::movelib::make_unique<vector<InboxMessage*>>();
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	int flag = 0;
	for(int i=to_seq;i>=from_seq && flag == 0;i--){
		db->pstmt = db->mysql_conn->prepareStatement("select * from user_inbox where node_id_seq = ?");
		stringstream ss;
		ss << *node_id << "_" << i;
		db->pstmt->setString(1, ss.str());
		db->res = db->pstmt->executeQuery();
		while(db->res->next()){

			ssize_t msg_timestamp = db->res->getInt64(COLUMN_INBOX_MESSAGES_TIMESTAMP);

			bool is_valid = msg_timestamp > timea && msg_timestamp < timeb;
			if(is_valid){
				InboxMessage *inbox_message = inbox_message_allocator==NULL?new InboxMessage() : (*inbox_message_allocator)();
				inbox_message->set_payload(db->res->getString(COLUMN_INBOX_MESSAGES_PAYLOAD));
				inbox_message->set_message_type(db->res->getInt(COLUMN_INBOX_MESSAGES_MESSAGE_TYPE));
				inbox_message->set_from_id(db->res->getString(COLUMN_INBOX_MESSAGES_FROM_ID));
				inbox_message->set_timestamp(msg_timestamp);

				inbox_messages->push_back(inbox_message);
			}
			else{
				flag = 1;
			}
		}
		if(inbox_messages->size()>100){
			more = true;
			break;
		}
		db->reset();//reset prepared statements
	}
	std::sort (inbox_messages->begin(), inbox_messages->end(), boost::bind(&DatabaseWrapper::inbox_sorting_comparator, this, boost::placeholders::_1,  boost::placeholders::_2));
	return inbox_messages;
}


long int DatabaseWrapper::get_seq(string *node_id, bool update){
	NodeSeqCache::ConstAccessor ac;
	shared_ptr<NodeSeq> node_seq = nullptr;
	long int now  = get_time_in_ms();
	bool just_fetched = false;
	if (node_seq_cache->find(ac, *node_id)) {
		node_seq =  *ac;
	}
	else{
		node_seq = boost::make_shared<NodeSeq>();
		if(!get_node_seq_from_db(node_id, node_seq)){//doesnt exist in db
			if(update){
				unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
				db->pstmt = db->mysql_conn->prepareStatement("insert into node_seq(node_id, seq, last_updated) values(? , ? , ?)");
				db->pstmt->setString(1, *node_id);
				db->pstmt->setInt64(2, 0);
				db->pstmt->setInt64(3, now);
				db->pstmt->executeUpdate();
				node_seq->last_updated = now;
				node_seq->seq = 0;
			}
			return	0;
		}
		just_fetched = true;
		node_seq_cache->insert(*node_id, node_seq);//update in cache
	}

	if(now - node_seq->last_updated >5*60*1000){//check if it has been updated in db
		long int ret = node_seq->seq;
		if(!just_fetched){
			get_node_seq_from_db(node_id, node_seq);
		}
		if(node_seq->seq <= ret && update){//none has updated
			node_seq->seq++;
			unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
			db->pstmt = db->mysql_conn->prepareStatement("update node_seq set seq = ? , last_updated = ?  where node_id = ?");
			db->pstmt->setInt64(1, node_seq->seq);
			db->pstmt->setString(3, *node_id);
			db->pstmt->setInt64(2, now);
			if(db->pstmt->executeUpdate()!=0){
				node_seq->last_updated = now;
			}
		}
	}
	return node_seq->seq;
}

bool DatabaseWrapper::get_node_seq_from_db(string *node_id, shared_ptr<NodeSeq> &node_seq){
	unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> db = get_db_pooled_connection(0);
	db->pstmt = db->mysql_conn->prepareStatement("select * from node_seq where node_id = ?");
	db->pstmt->setString(1, *node_id);
	db->res = db->pstmt->executeQuery();
	while(db->res->next()){
		node_seq->seq = db->res->getInt64(COLUMN_NODE_SEQ_SEQ);
		node_seq->last_updated = db->res->getInt64(COLUMN_NODE_SEQ_LAST_UPDATED);
		return true;
	}
	return false;
}

// connection related functions here
MysqlDbConnection* DatabaseWrapper::_get_db_connection(const MysqlDbInstance *db_instance){
	size_t retry_count = 500;

	MysqlDbConnection *ret = NULL;
	while(true){
		while(!db_instance->connection_pool.try_pop(ret)){

				char ip_addr[16];
				if(convert_host_name_to_ip(db_instance->addr.c_str(), ip_addr)){
					ret = MysqlDbConnection::getInstance( ip_addr, db_instance->db_name, db_instance->db_user_name, db_instance->db_password);
				}
				if(ret==NULL){
					server_logger->error("unable to connect to db, sleeping for {} ms", retry_count);
					struct pollfd pf = { 0 };
					pf.fd = -1;
					poll( &pf,1,retry_count*=2);
				}
				else{
					db_instance->connection_pool.push(ret);
				}
				if(retry_count>256000){//256s //db down or something
					server_logger->error("Db connection error : {}", db_instance->instance_id);
					throw sql::SQLException("Connection timed out");
				}
		}
		if(ret->mysql_conn->isValid() || ret->mysql_conn->reconnect()){
			break;
		}
		else{
			//drop this connection
			ret->reset();
			delete ret->mysql_conn;
			delete ret;
		}

	};
	return ret;
}

void DatabaseWrapper::_release_db_connection(const MysqlDbInstance *db_instance, MysqlDbConnection *conn){
	if(!conn){
		return;
	}
	conn->reset();
	db_instance->connection_pool.push(conn);
}

unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>> DatabaseWrapper::get_db_pooled_connection(int instance_id){

	concurrent_hash_map<int, MysqlDbInstance*>::const_accessor db_instance_accessor;
	while(!db_instances_set.find(db_instance_accessor, instance_id)){
		//TODO: try to load from local , if not try to load from db, default instance should hold the id to instance map too
		server_logger->error("not implemented yet");
	}

	const MysqlDbInstance *db_instance = db_instance_accessor->second;
	//release it
	db_instance_accessor.release();

	return unique_ptr<MysqlDbConnection, boost::function<void(MysqlDbConnection*)>>(_get_db_connection(db_instance) , boost::bind(&DatabaseWrapper::_release_db_connection, this, db_instance, boost::placeholders::_1));
}

void DatabaseWrapper::close_all_connections(){
	concurrent_hash_map<int, MysqlDbInstance*>::iterator it = db_instances_set.begin();

	while(it!=db_instances_set.end()){
		MysqlDbInstance *db_instance = it->second;
		MysqlDbConnection * ret;
		while(db_instance->connection_pool.try_pop(ret)){
			ret->reset();
			ret->mysql_conn->close();
			delete ret->mysql_conn;
			delete ret;
		}
		it++;
	}
}

void DatabaseWrapper::clear_all_caches(){
	node_cache->clear();
	session_cache->clear();
	session_node_cache->clear();
	node_seq_cache->clear();

}

