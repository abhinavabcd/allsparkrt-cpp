/*
 * config.h
 *
 *  Created on: 16-Jan-2017
 *      Author: abhinav
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include<string>
#include "boost/move/unique_ptr.hpp"
#include "boost/move/make_unique.hpp"


using boost::movelib::unique_ptr;
using boost::movelib::make_unique;
using std::string;


typedef struct Config{
	string gcm_authorization_key;

	int default_db_instance_id;
	string default_db_host;
	string default_db_port ;

	string default_db_username;
	string default_db_password;
	string default_db_name;

	string server_secret_key;


	bool config_enable_flush_all_messages_to_db = false;

} Config;


extern Config server_config;



void load_config_from_json(const char *filename);

#endif /* CONFIG_H_ */
