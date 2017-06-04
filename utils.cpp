/*
 * utils.cpp
 *
 *  Created on: 30-Dec-2016
 *      Author: abhinav
 */





#include "utils.h"
#include "utilities/conversion_utils/conversion_utils.h"
#include "server.h"


using json = nlohmann::json;
using boost::shared_ptr;
using allspark::ConfigRequest;
using boost::make_shared;
using std::string;
using std::max;


static const string alphanum =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static const int all_characters_length = alphanum.length();


static const long int SOMETIME_WHEN_WE_STARTED = 1496040323000;
//random id generator based on shard id , first 42 bits are timestamp, next 22 bits contain shard and other info
int gen_random(char *s, int shard_id) {
	//mask 42 bits->move 22 bits for shard_id and other info
	unsigned long long int id = (((1LL<<42) - 1)&(get_time_in_ms() - SOMETIME_WHEN_WE_STARTED)) << 22;

	//some random shit chnage later to counters
	id |= ((((1<<10) -1 ) & (rand() %(1<<10))) << 12) ;//12

	id |= (((1<<12) -1 ) & shard_id);//12

	return itoa(id, s, 10);
}

char* gen_random_string(char *s, const int len) {
    for (int i = 0; i < len; ++i) {
        s[i] = alphanum[rand() % all_characters_length];
    }
   return s;
}




void create_json_from_message(shared_ptr<WrappedMessage>& msg, json &j){
	if(!msg->dest_session_id().empty()){
		j["dest_session_id"] = msg->dest_session_id();
	}
	if(!msg->dest_id().empty()){
		j["dest_id"] = msg->dest_id();
	}
	if(!msg->payload().empty()){
		j["payload"] = msg->payload();
	}
	if(!msg->payload1().empty()){
		j["payload1"] = msg->payload1();
	}
	if(!msg->id().empty()){
		j["id"]  = msg->id();
	}
	if(!msg->payload2().empty()){
		j["payload2"] = msg->payload2();
	}
	j["type" ]  = msg->type();
	j["timestamp"] = msg->timestamp();

	if(!msg->src_id().empty()){
		j["src_id" ] =  msg->src_id();
	}
}

void load_config_message_from(string *payload, ConfigRequest &config_request){
	json j = json::parse(*payload);

	if(j["update_gcm_key"]!=nullptr){
		config_request.set_update_gcm_key(j["update_gcm_key"]);
	}
	if(j["fetch_inbox_messages"]!=nullptr){
		config_request.set_fetch_inbox_messages(j["fetch_inbox_messages"]);
	}
	if(j["timestamp"]!=nullptr){
		config_request.set_timestamp(j["timestamp"]);
	}
	if(j["fetch_inbox_messages_from_time_stamp"]!=nullptr){
		config_request.set_fetch_inbox_messages_from_timestamp(j["fetch_inbox_messages_from_time_stamp"]);
	}
	if(j["fetch_inbox_messages_to_seq"]!=nullptr){
		config_request.set_fetch_inbox_messages_to_seq(j["fetch_inbox_messages_to_seq"]);
	}

	if(j["fetch_inbox_messages_from_seq"]!=nullptr){
		config_request.set_fetch_inbox_messages_from_seq(j["fetch_inbox_messages_from_seq"]);
	}
}


inline void load_inbox_message_to_json(const allspark::InboxMessage &from_message, json &to_message){
	shared_ptr<WrappedMessage> m = make_shared<WrappedMessage>();
	m->ParseFromString(from_message.payload());
	create_json_from_message(m , to_message);
}


//rvo should do the move semantics on the string
string load_config_response_to_json(allspark::ConfigResponse &response){
	json j;

	json message_array= json::array();

	size_t max = 0;
	for(int i=0;i<response.messages_size();i++){
		//each inbox message to json
		const allspark::InboxMessage& message =  response.messages(i);
		json json_message;
		load_inbox_message_to_json(message, json_message);
		message_array.push_back(json_message.dump());
		max = std::max<size_t>(max, message.timestamp());
	}
	j["messages"] = message_array;
	j["from_seq"] = response.from_seq();
	j["to_seq"] = response.to_seq();
	j["more"] = response.more();
	j["server_timestamp_add_diff"] = response.user_time_stamp_diff();
	j["last_timestamp"] = max;

	return j.dump();
}


void load_message_from_json(json &j, shared_ptr<WrappedMessage> &message){
	if (j["dest_id"]!=nullptr)
		message->set_dest_id(j["dest_id"]);
	if (j["src_id"]!=nullptr)
		message->set_src_id(j["src_id"]);
	if (j["dest_session_id"]!=nullptr)
		message->set_dest_session_id(j["dest_session_id"]);
	if (j["type"]!=nullptr)
		message->set_type(j["type"]);
    if (j["payload"]!=nullptr)
		message->set_payload(j["payload"]);
    if (j["payload1"]!=nullptr)
		message->set_payload1(j["payload1"]);
    if (j["payload2"]!=nullptr)
		message->set_payload2(j["payload2"]);
    if (j["id"]!=nullptr)
		message->set_id(j["id"]) ;
    if (j["is_ack_required"]!=nullptr)
		message->set_is_ack_required(j["is_ack_required"]);
    if (j["anonymize_src_id"]!=nullptr)
		message->set_is_anonymize_src_id(j["anonymize_src_id"]);
    if (j["anonymize_src_id"]!=nullptr)
		message->set_anonymize_src_id(j["anonymize_src_id"]);
    if (j["timestamp"]!=nullptr)
		message->set_timestamp(j["timestamp"]);
}
