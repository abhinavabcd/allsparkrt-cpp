/*
 * auth_utils.cpp
 *
 *  Created on: Dec 20, 2016
 *      Author: abhinav
 */




#include "auth_utils.h"
#include "http_and_websocket_utils.h"
#include "utilities/HMAC_SHA1/HMAC_SHA1.h"
#include "wire_messages/wire_transfers.pb.h"
#include <iostream>
#include "utilities/conversion_utils/conversion_utils.h"
#include "config.h"
/*
 * Auth_key utils
 */

using std::string;

#ifndef USE_TORNADO_AUTH_KEY

int generate_auth_key(string &node_id, char *auth_key, int &auth_key_len){
		allspark::AuthData auth_obj;
		auth_obj.set_node_id(node_id);
		int auth_data_length = auth_obj.ByteSize();
		char auth_data[auth_data_length];
		auth_obj.SerializeToArray(auth_data, auth_data_length);//protobuf

		size_t digest_length = 20;//server_secret_key.length();
		CHMAC_SHA1 hmac_sha1;
		char digest_calculated[digest_length];
		hmac_sha1.HMAC_SHA1((unsigned char *)auth_data, auth_data_length, server_config.server_secret_key.c_str(), server_config.server_secret_key.length(), (unsigned char *) digest_calculated);

		size_t l1, l2;
		char raw_auth_key[100];
		//join websocket frames of digest + auth_data(protobuf)
		make_websocket_frame(WebSocketFrameType::TEXT_FRAME, digest_calculated, digest_length, raw_auth_key, l1);
		make_websocket_frame(WebSocketFrameType::TEXT_FRAME, auth_data, auth_data_length, raw_auth_key+l1, l2);
		//now calculate base64 inplace
		return (auth_key_len = base64_encode(raw_auth_key, l1+l2, auth_key));
}


bool retrieve_auth(char *auth_key, size_t auth_key_len , allspark::AuthData &auth_obj){
	//url decode inplace

	char decoded_buf[auth_key_len];
	int decoded_length = base64_decode(auth_key , auth_key_len, decoded_buf);

	char *digest;
	int digest_length;
	char *auth_data;
	int auth_data_length;
	if(get_websocket_frame(decoded_buf, decoded_length, digest, digest_length, NULL) != TEXT_FRAME || get_websocket_frame(digest+digest_length, decoded_length - (digest+digest_length - decoded_buf), auth_data, auth_data_length, NULL) != TEXT_FRAME){
		return false;
	}

	CHMAC_SHA1 HMAC_SHA1;
	char digest_calculated[server_config.server_secret_key.length()];
	//calculates a digest with a key
	HMAC_SHA1.HMAC_SHA1((unsigned char *)auth_data, auth_data_length, server_config.server_secret_key.c_str(), server_config.server_secret_key.length(), (unsigned char *) digest_calculated);

	if(memcmp(digest_calculated, digest, digest_length)!=0){
		//invalid request
		return false;
	}
	auth_obj.ParseFromArray(auth_data, auth_data_length);//protobuf
	return true;
}

#else

/*
 * def create_signed_value(server_secret , name, value):
    timestamp = utf8(str(int(time.time())))
    value = base64.b64encode(utf8(value))
    signature = _create_signature(server_secret, name, value, timestamp)
    value = b"|".join([value, timestamp, signature])
    return value

def decode_signed_value(server_secret, name, value, max_age_days=31):
    if not value:
        return None
    parts = utf8(value).split(b"|")
    if len(parts) != 3:
        return None
    signature = _create_signature(server_secret, name, parts[0], parts[1])
    if not _time_independent_equals(parts[2], signature):
        logging.warning("Invalid cookie signature %r", value)
        return None

    if parts[1].startswith(b"0"):
        logging.warning("Tampered cookie %r", value)
        return None
    try:
        return base64.b64decode(parts[0])
    except Exception:
        return None


def _create_signature(secret, *parts):
    hash = hmac.new(utf8(secret), digestmod=hashlib.sha1)
    for part in parts:
        hash.update(utf8(part))
    return utf8(hash.hexdigest())
 */

void get_hex_digest(char *node_id, int node_id_len, char * timestamp, int timestamp_len, char*hex_digest){
	CHMAC_SHA1 hmac_sha1;
	unsigned char digest_calculated[20];
	//translate key 5c.
	char translate_table[256];
	for(int i=0;i<256;i++){
		translate_table[i] = i^0x36;
	}
	char translated_key[64];
	int n = server_config.server_secret_key.length();
	for(int i=0;i< 64;i++){
		if(i<n){
			translated_key[i] = translate_table[server_config.server_secret_key[i]];
		}
		else{
			translated_key[i] = translate_table[0];
		}
	}


	hmac_sha1.Reset();
	hmac_sha1.Update((UINT_8 *)translated_key, 64);
	hmac_sha1.Update((UINT_8 *)"user_key", 8);
	hmac_sha1.Update((UINT_8 *)node_id, node_id_len);
	hmac_sha1.Update((UINT_8 *)timestamp, timestamp_len);
	hmac_sha1.Final();
	hmac_sha1.GetHash((UINT_8 *)digest_calculated);

	/*
	 * step 2 outer key
	 */

	for(int i=0;i<256;i++){
		translate_table[i] = i^0x5C;
	}
	for(int i=0;i<64;i++){
		if(i<n){
			translated_key[i] = translate_table[server_config.server_secret_key[i]];
		}else{
			translated_key[i] = translate_table[0];
		}
	}

	hmac_sha1.Reset();
	//translate key 0x36.
	hmac_sha1.Update((UINT_8 *)translated_key, 64);
	hmac_sha1.Update((UINT_8 *)digest_calculated, 20);
	hmac_sha1.Final();
	hmac_sha1.GetHash((UINT_8 *)digest_calculated);



	for(int i=0;i<20;i++){
		hex_digest[2*i] = to_hex(digest_calculated[i] >> 4);
		hex_digest[2*i+1]= to_hex(digest_calculated[i] & 15);
	}

}

int generate_auth_key( string &node_id, char *auth_key, int &auth_key_len){

		char timestamp[20];
		int timestamp_len = itoa((long int)get_time_in_ms()/1000,timestamp,  10);

		char hex_digest[40];
		int hex_digest_len = 40;

		char node_id_b64[100];
		int node_id_b64_len = base64_encode(&node_id[0], node_id.length(), node_id_b64);
		get_hex_digest(node_id_b64, node_id_b64_len, timestamp, timestamp_len, hex_digest);
		//use the hex digest
		int s = 0;
		memcpy(auth_key, node_id_b64, node_id_b64_len);s+=node_id_b64_len;
		auth_key[s++] = '|';
		memcpy(auth_key+s, timestamp,timestamp_len );s+=timestamp_len;
		auth_key[s++] = '|';
		memcpy(auth_key+s, hex_digest, hex_digest_len );s+=hex_digest_len;
		return auth_key_len = s;
}

inline int find(char x, char *s, int l){
	for(int i=0;i<l;i++){
		if(s[i]==x){
			return i;
		}
	}
	return -1;
}
bool retrieve_auth( char *auth_key, size_t auth_key_len , allspark::AuthData &auth_obj){
	//url decode inplace
	StringPtr parts[3];

	char *_auth_key = auth_key;
	int _auth_key_len = auth_key_len;
	for(int i=0;i<3;i++){
		int offset = find('|', _auth_key , _auth_key_len);
		if(offset == -1 && i!=2){
			return false;
		}
		if(i==2){
			offset = _auth_key_len;
		}
		parts[i].ptr = _auth_key;
		parts[i].len = offset;
		_auth_key = _auth_key+offset+1;
		_auth_key_len = _auth_key_len - offset-1;
	}
	char hex_digest[40];
	int hex_digest_len = 40;

	get_hex_digest(parts[0].ptr, parts[0].len, parts[1].ptr, parts[1].len, hex_digest);
	if(strncmp(hex_digest , parts[2].ptr , parts[2].len)==0){
		int l = base64_decode(parts[0].ptr, parts[0].len, parts[0].ptr);
		auth_obj.set_node_id(parts[0].ptr, l);
		return true;
	}
	return false;
}


#endif
