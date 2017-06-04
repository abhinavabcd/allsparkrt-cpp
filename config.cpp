/*
 * config.cpp
 *
 *  Created on: 16-Jan-2017
 *      Author: abhinav
 */


#include <fstream>
#include <string>
#include "config.h"
#include "utilities/json/json.hpp"


using std::string;
using json = nlohmann::json;


Config server_config;


//default constant

void load_config_from_json(const char *filename){


//   FILE* f = fopen(filename, "r");
//   fseek(f, 0, SEEK_END);
//   size_t size = ftell(f);
//   char config_data[size];
//   rewind(f);
//   fread(config_data, sizeof(char), size, f);
//   fclose(f);


   std::ifstream ifs(filename);
   string config_data( (std::istreambuf_iterator<char>(ifs) ),
	                       (std::istreambuf_iterator<char>()    ) );
   ifs.close();

   json config_obj = json::parse(config_data.begin(), config_data.end());


   server_config.gcm_authorization_key = config_obj["gcm_authorization_key"];

   server_config.default_db_instance_id = config_obj["default_db_instance_id"];
   server_config.default_db_host = config_obj["default_db_host"];
   server_config.default_db_port = config_obj["default_db_port"];

   server_config.default_db_username = config_obj["default_db_username"];
   server_config.default_db_password = config_obj["default_db_password"];
   server_config.default_db_name = config_obj["default_db_name"];

   server_config.server_secret_key = config_obj["server_secret_key"];
   server_config.config_enable_flush_all_messages_to_db = config_obj["config_enable_flush_all_messages_to_db"];

}
