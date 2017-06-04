/*
 * flush_messages_to_db.cpp
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */


#include <thread>


#include "flush_messages_to_db.h"
#include "../libco/co_routine.h"
#include "../libco/co_routine_inner.h"
#include "../mysql_db.h"
#include "../utils.h"
#include "spdlog/spdlog.h"
#include "../config.h"
#include "../constants.h"

extern tbb::concurrent_queue<shared_ptr<WrappedMessage>> flush_to_db_queue;
extern concurrent_queue< tuple< string, shared_ptr<WrappedMessage> , int> > inbox_pending_messages_queue;


extern DatabaseWrapper db;
extern std::shared_ptr<spdlog::logger> server_logger;
extern bool is_server_running;

//just loop over the queue to push to db
void *flush_to_db_task(void *arg){
	co_enable_hook_sys();
	while(true){
		shared_ptr<WrappedMessage> msg = nullptr;
		while(!flush_to_db_queue.try_pop(msg)){
			if(!is_server_running){
				return NULL;
			}//sleep for second
			struct pollfd pf = { 0 };
			pf.fd = -1;
			poll( &pf,1,1000);
		}

		try{
			//this is place message appropriately in the sessions, so that we can restore by sessions
			db.add_message_to_inbox(NULL, &(*msg));
		}
		catch(sql::SQLException &ex){
			server_logger->error("mysql error {} {} line {}" ,ex.getErrorCode() , ex.what() ,__LINE__);
		}
	}
}



void *flush_pending_messages(void *arg){
	co_enable_hook_sys();
	while(true){
		tuple<string, shared_ptr<WrappedMessage> , int> dest_id_and_message;
		while(!inbox_pending_messages_queue.try_pop(dest_id_and_message)){
			if(!is_server_running){
				return NULL;
			}//sleep for second
			struct pollfd pf = { 0 };
			pf.fd = -1;
			poll( &pf,1,1000);
		}
		string &dest_id = dest_id_and_message.get<0>();
		shared_ptr<WrappedMessage> &msg = dest_id_and_message.get<1>();
		int message_sending_flags = dest_id_and_message.get<2>();

		long int now = msg->timestamp()>0?msg->timestamp():get_time_in_ms();
		try{
			int type = 0;
			const string gcm_key = db.get_push_key(dest_id, type);

			if(msg->is_anonymize_src_id() && msg->data2.ptr){
				db.add_pending_messages( &dest_id, msg->mutable_src_id(),  msg->type(), msg->data2.ptr ,  msg->data2.len , now);
			}
			else if(msg->data1.ptr){
				db.add_pending_messages(&dest_id, msg->mutable_src_id(),  msg->type(),  msg->data1.ptr ,  msg->data1.len ,now);
			}

			if(msg->type()>0 && (message_sending_flags & IS_RESENDING_ASSUMED_SENT)){
				//no delivery based stuff
				send_a_push_notification(gcm_key, &dest_id, msg->payload(), msg->type());
			}
		}
		catch(sql::SQLException &ex){
			server_logger->error("mysql error {} {} line {}" ,ex.getErrorCode() , ex.what() ,__LINE__);
		}
	}
}


int check_if_server_running(void *){
	if(is_server_running || get_co_routines_count()>0){
		return 0;
	}
	return -1;
}

void *start_looper( void *arg){
	stCoEpoll_t * ev = co_get_epoll_ct(); //ct = current thread
	//start flushing tasks
	stCoRoutine_t* co_flush_messages_to_db[10];
	stCoRoutine_t* co_flush_pending[10];
	if(server_config.config_enable_flush_all_messages_to_db){
		server_logger->info("starting flushing to db coroutines.");
		for (int i = 0; i < 10; i++){
			co_create(&co_flush_messages_to_db[i], NULL, flush_to_db_task, NULL);
			co_resume(co_flush_messages_to_db[i]);
		}
	}

	server_logger->info("starting flushing pending messages  and push notifications coroutines.");
	for (int i = 0; i < 10; i++){
		co_create(&co_flush_pending[i], NULL, flush_pending_messages, NULL);
		co_resume(co_flush_pending[i]);
	}

	co_eventloop( ev, check_if_server_running ,0 );

	if(server_config.config_enable_flush_all_messages_to_db){
		for (int i = 0; i < 10; i++){
			co_free(co_flush_messages_to_db[i]);
		}
	}

	for (int i = 0; i < 10; i++){
		co_free(co_flush_pending[i]);
	}

	return NULL;
}

pthread_t start_flush_to_db_thread(){

	pthread_t tid;
	pthread_create( &tid,NULL,start_looper,0);
	return tid;
}
