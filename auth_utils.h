/*
 * auth_utils.h
 *
 *  Created on: Dec 20, 2016
 *      Author: abhinav
 */

#ifndef AUTH_UTILS_H_
#define AUTH_UTILS_H_

#include <string>
#include "wire_messages/wire_transfers.pb.h"

using std::string;

int generate_auth_key(string &node_id, char *auth_key, int &auth_key_len);
bool retrieve_auth( char *auth_key, size_t auth_key_len , allspark::AuthData &auth_obj);

#endif /* AUTH_UTILS_H_ */


