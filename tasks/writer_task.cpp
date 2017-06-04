/*
 * writer_task.cpp
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */

#include "writer_task.h"
#include "../constants.h"
#include "spdlog/spdlog.h"

extern std::shared_ptr<spdlog::logger> server_logger;




void add_to_assumed_sent_messages(Connection *conn, shared_ptr<WrappedMessage>& msg, size_t now){
	while(!conn->msg_assumed_sent.empty()){
		pair<size_t , shared_ptr<WrappedMessage>> &assumed_sent = conn->msg_assumed_sent.front();
		if(now - assumed_sent.first > _MAX_TCP_USER_TIMEOUT*1000){
			server_logger->info("poping old messages ");
			assumed_sent.second = nullptr; // destrory forcefully
			conn->msg_assumed_sent.pop();//calls the destructor ?
		}
		else{
			break;
		}
	}

	if(!(msg->type()==CLIENT_CONFIG_RESPONSE || msg->type()==0 || msg->type()==USER_OFFLINE_RESPONSE)){//need not save system type messages !
		conn->msg_assumed_sent.push(std::move(pair<size_t, shared_ptr<WrappedMessage>>(now, std::move(msg))));//copy the shared ptr
	}
}

void *write_data_to_connection(void *arg){
	co_enable_hook_sys();
	WriterTask* task = arg;
	while(true){
		Connection * conn = task->conn;
		if(!conn){
			task->thread_handle->writer_tasks_pool.push(task);//put back into pool and sleep
			co_yield_ct();
			continue;
		}
		conn->active_writer = task;
		while(!conn->pending_queue.empty()){

			shared_ptr<WrappedMessage> &msg = conn->pending_queue.front();
			size_t len = 0;
			char header_data[10]; //websocket header

			if(conn->data_io_type==PROTOBUF_IO_TYPE){
				if(conn->is_external_node && msg->is_anonymize_src_id()){
					size_t frame_header_size = make_websocket_frame(WebSocketFrameType::TEXT_FRAME, NULL, msg->data2.len, header_data, len , true);
					if(write_ssl_auto_handshake(conn->ssl, header_data, frame_header_size, conn->is_ssl)<0 ||
							write_ssl_auto_handshake(conn->ssl, msg->data2.ptr, msg->data2.len, conn->is_ssl)<0){
						break;
					}
				}
				else{
					size_t frame_header_size = make_websocket_frame(WebSocketFrameType::TEXT_FRAME, NULL, msg->data1.len, header_data, len , true);
					if(write_ssl_auto_handshake(conn->ssl, header_data, frame_header_size , conn->is_ssl)<0 ||
							write_ssl_auto_handshake(conn->ssl, msg->data1.ptr, msg->data1.len, conn->is_ssl)<0){
						break;
					}
				}
			}
			//handle json out clients
			else if(conn->data_io_type==JSON_IO_TYPE){
				if(conn->is_external_node && msg->is_anonymize_src_id()){

					//json
					json j;
					create_json_from_message( msg, j);
					j["src_id" ] =  msg->anonymize_src_id(); //replace it
					string json_data = j.dump();
					size_t frame_header_size = make_websocket_frame(WebSocketFrameType::TEXT_FRAME, NULL, json_data.length(), header_data, len , true);
					if(write_ssl_auto_handshake(conn->ssl, header_data, frame_header_size, conn->is_ssl)<0 || write_ssl_auto_handshake(conn->ssl, &json_data[0], json_data.length(), conn->is_ssl)<0){
						break;
					}
				}
				else{

					//json
					json j;
					create_json_from_message( msg, j);
					string json_data = j.dump();
					size_t frame_header_size = make_websocket_frame(WebSocketFrameType::TEXT_FRAME, NULL, json_data.length(), header_data, len , true);
					if(write_ssl_auto_handshake(conn->ssl, header_data, frame_header_size, conn->is_ssl)<0 ||  write_ssl_auto_handshake(conn->ssl, &json_data[0], json_data.length(), conn->is_ssl)<0){
						break;
					}
				}
			}
			server_logger->info("Writer task send to node_id {}", conn->to_node_id);
			size_t now = conn->last_msg_sent_timestamp  = get_time_in_ms();
			if(conn->should_save_resending_buffer){
				add_to_assumed_sent_messages(conn, msg, now);//msg will be moved
			}
			msg = nullptr;
			conn->pending_queue.pop();
		}
		conn->active_writer = NULL;
		task->conn = NULL;
	}
}
/*
 * start a writer task after pushing data onto connection pending queue , that will keep writing
 */
void start_writer_task(Connection *conn, ThreadHandle * thread_handle){
	if(!conn || conn->active_writer){ //already a writer is active
		return;
	}
	WriterTask *ret;
	if(thread_handle->writer_tasks_pool.empty()){
		//try creating new writer task
		ret = new WriterTask();

		stCoRoutineAttr_t attr;
		attr.stack_size = 0;
		attr.share_stack = thread_handle->co_share_stack;
		server_logger->info("creating writer task");
		co_create(&ret->co_routine , &attr, write_data_to_connection , ret);
	}
	else{
		ret = thread_handle->writer_tasks_pool.front();
		thread_handle->writer_tasks_pool.pop();
	}
	ret->conn = conn;
	ret->thread_handle = thread_handle;
	co_resume(ret->co_routine);
}
