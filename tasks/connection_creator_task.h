/*
 * connection_creator_task.h
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */

#ifndef CONNECTION_CREATOR_TASK_H_
#define CONNECTION_CREATOR_TASK_H_


#include "boost/shared_ptr.hpp"
#include "boost/move/unique_ptr.hpp"
#include "../server.h"



using boost::shared_ptr;
using boost::movelib::unique_ptr;

void start_connection_creator_task(shared_ptr<Node> &node, shared_ptr<WrappedMessage> &msg, ThreadHandle *thread_handle);


#endif /* CONNECTION_CREATOR_TASK_H_ */
