/*
 * http_calls.h
 *
 *  Created on: 22-Dec-2016
 *      Author: abhinav
 */

#ifndef HTTP_CALLS_H_
#define HTTP_CALLS_H_


#include <string.h>
#include <map>
#include "utils.h"
#include "picohttpparser/picohttpparser.h"
#include "mysql_db.h"
#include "mem_utils.h"
#include "ssl_utils.h"

extern DatabaseWrapper db;
extern MemRecycler mem_recycler;
extern int SESSION_JOINED;


using std::string;
using std::map;

/*
 *  http server calls
 */

int get_last_seen_timestamp(Request *req);

int websocket_handler_v3( Request *req);
int join_session( Request *req);
int unjoin_session( Request *req);
int reveal_anonymity( Request *req);
int get_session_info( Request *req);
int get_sessions(Request *req);


int create_session( Request *req);
int push_message( Request *req);


int fetch_inbox_messages(Request *req);
int add_to_inbox(Request *req);

#endif /* HTTP_CALLS_H_ */
