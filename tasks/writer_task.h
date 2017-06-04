/*
 * writer_task.h
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */

#ifndef WRITER_TASK_H_
#define WRITER_TASK_H_




#include "boost/shared_ptr.hpp"
#include "boost/move/unique_ptr.hpp"
#include "../server.h"



using boost::shared_ptr;
using boost::movelib::unique_ptr;

void start_writer_task(Connection *conn, ThreadHandle * thread_handle);


#endif /* WRITER_TASK_H_ */
