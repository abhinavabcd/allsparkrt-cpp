/*
 * flush_messages_to_db.h
 *
 *  Created on: 24-Feb-2017
 *      Author: abhinav
 */

#ifndef FLUSH_MESSAGES_TO_DB_H_
#define FLUSH_MESSAGES_TO_DB_H_


#include "tbb/concurrent_queue.h"
#include "boost/shared_ptr.hpp"
#include "../server.h"
using boost::shared_ptr;

extern tbb::concurrent_queue<shared_ptr<WrappedMessage>> flush_to_db_queue;

pthread_t start_flush_to_db_thread();

#endif /* FLUSH_MESSAGES_TO_DB_H_ */
