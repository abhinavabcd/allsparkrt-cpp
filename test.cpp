/*
 * test.cpp
 *
 *  Created on: Dec 6, 2016
 *      Author: abhinav
 */


#include "utilities/HMAC_SHA1/HMAC_SHA1.h"
#include<string.h>
#include<iostream>
#include<cstdlib>
#include<map>
#include<boost/shared_ptr.hpp>
#include<boost/make_shared.hpp>
#include "server.h"
#include "wire_messages/wire_transfers.pb.h"
#include "auth_utils.h"
#include "mysql_db.h"
#include "boost/regex.hpp"
#include "boost/move/unique_ptr.hpp"
#include "boost/bind.hpp"
#include "boost/bind/placeholders.hpp"
#include "mem_utils.h"
#include "boost/function.hpp"
#include "http_and_websocket_utils.h"
#include "config.h"
#include "utils.h"
#include "utilities/conversion_utils/conversion_utils.h"

using std::string;
using std::cout;
using std::endl;
using std::map;
using boost::shared_ptr;


extern MemRecycler mem_recycler;

int sha_test(){
	const char Key[20]={'a','b','h','i','n','a','v'};
	BYTE digest[20];

	char * test = "auth_key\0";

	CHMAC_SHA1 HMAC_SHA1;
	HMAC_SHA1.HMAC_SHA1((unsigned char *)test, strlen(test), Key, sizeof(Key), digest);
	cout << digest << endl;

	HMAC_SHA1.HMAC_SHA1((unsigned char *)test, strlen(test), Key, sizeof(Key), digest);
	cout << digest << endl;
	// base64_encode(websocketframeof(digest)+websocketframe(key:value))
	// base64_decode(value) , then extract websocket frames, with data



}

struct Dummy{
	int i;
};


int parse_path(char *path , int path_len , int &folder_path_length, map<string, StringPtr>& query_params){
	size_t query_start = path_len;
	for(int i=0;i<path_len;i++){
		if(path[i]=='?'){
			folder_path_length = i;
			query_start = i+1;
			break;
		}
	}
	size_t last_equalto_token = -1;
	size_t prev_start = query_start;

	for(int i=query_start;i<path_len;i++){
		if(path[i]=='&' || i==path_len-1){
			StringPtr s;//created on stack
			s.ptr = (char *)(path+last_equalto_token+1);
			s.len = i-last_equalto_token- (i==path_len-1 ? 0 : 1);
			query_params[string(path+prev_start, last_equalto_token-prev_start)] = s;//copies the structure into the maps internal tree
			prev_start = i+1;
			last_equalto_token = -1;
			continue;
		}
		else if(path[i]=='=' && last_equalto_token==-1){
			last_equalto_token = i;
		}
	}
}


shared_ptr<StringPtr> test_pass_by_ref(const string &t){
	if(&t==NULL){
		cout << "sad fuck!!!" << endl;
	}
	else{
		cout << "happy fuck!!!" << endl;
	}
}


void test_convert_host_to_ip(){
	char ip_addr[16];
	string google = "google.com";
	convert_host_name_to_ip(google.c_str(), ip_addr);
	cout << ip_addr << endl;
}


class Dummy2{
public:
	int i;
	Dummy2():i(4){
		cout << "constructed dummy" << endl;
	}
	~Dummy2(){
		cout << "destructed" << endl;
	}
};

extern DatabaseWrapper db;

int main(){
	std::srand (std::time (0));

	sha_test();
	map<string, StringPtr> m;
	int f = 0;
	char * s = "/helloworld.php?a=bello&c=dello";
	parse_path(s, strlen(s), f, m);
	cout << endl;
	for(map<string, StringPtr>::iterator it = m.begin();it!=m.end();it++){
		cout << it->first <<  " : " << string(it->second.ptr, it->second.len) << endl;
	}
	cout << f << endl;

	shared_ptr<StringPtr> p = test_pass_by_ref("hello");
	if(p==NULL){
		cout << "yes" << endl;
	}
	else{
		cout << "no" << endl;
	}
	string *test = new string("what the");
	std::pair<string, int> s2 = std::make_pair(*test, 100);

	delete test;
	cout << s2.first <<" - " <<s2.second << endl;


//	char auth_key[100];
//	int auth_key_length;
//	string node_id = "abhinav";
//	generate_auth_key(node_id , auth_key , auth_key_length);
//	cout << string(auth_key, auth_key_length) << endl;

	shared_ptr<Dummy2> d=  boost::make_shared<Dummy2>();
	shared_ptr<Dummy2> &c = d;
	cout << c->i << endl;

	pair<int , shared_ptr<Dummy2>> q = std::make_pair(1, c);

	cout << q.second.use_count() << endl;
	cout << c.use_count() << endl;
	if(c){
		cout << "true" << endl;
	}
	else{
		cout << "false" << endl;
	}

	cout << "### auth utils test ###" << endl;
	char a[500];
	int l;
	string to_encrypt = "yclHiZCjviIlk1D";
	generate_auth_key(to_encrypt, a, l);
	cout << string(a, l) << endl;
	allspark::AuthData auth_obj;
	retrieve_auth(a, l , auth_obj);
	cout << auth_obj.node_id() << endl;

	to_encrypt = "sindhu";
	generate_auth_key(to_encrypt, a, l);
	cout << string(a, l) << endl;

	to_encrypt = "hello";
	generate_auth_key(to_encrypt, a, l);
	cout << string(a, l) << endl;

	to_encrypt = "hello2";
	generate_auth_key(to_encrypt, a, l);
	cout << string(a, l) << endl;

	string r = "ZnVjayB5bw==|1483550637|775f3754f55d505face69712decaf9d21b57b17d";
	if(!retrieve_auth(&r[0] , r.length() , auth_obj)){
		cout << "its fucked" <<endl;
	}
	else{
		cout << auth_obj.node_id() << endl;
	}


	cout << "### auth utils test end###" << endl;


//	cout << "## mysql  test###" << endl;
//
//	typedef tstarling::ThreadSafeStringKey String;
//	typedef String::HashCompare HashCompare;
//	typedef tstarling::ThreadSafeLRUCache<String, shared_ptr<Node>, HashCompare> NodeCache;
//	unique_ptr<NodeCache> node_cache = unique_ptr<NodeCache>(new NodeCache(100000));
//
//	shared_ptr<Node> node = db.create_node(NULL , "127.0.0.1","8081",false);
//	cout << node->node_id << endl;
//	shared_ptr<Node> node2 =  db.get_node_by_id(node->node_id, true, false);
//	if(node==node2){
//		cout << "yeah same from cache" << endl;
//	}
//	shared_ptr<Node> node3 = db.get_node_by_id(node->node_id, true, true);
//	if(node->addr == node3->addr){
//		cout << "yeah address same cool stuff" << endl;
//	}
//	cout << "### mysql test end###" << endl;

	cout << "sha digest check" <<endl;

	unsigned char digest_calculated[20];
	unsigned char hex_digest[40];


	CHMAC_SHA1 hmac_sha1;
	hmac_sha1.Reset();
	hmac_sha1.Update((UINT_8 *)"hello", 5);
	hmac_sha1.Final();

	hmac_sha1.GetHash((UINT_8 *)digest_calculated);



	for(int i=0;i<20;i++){
		hex_digest[2*i] = to_hex(digest_calculated[i] >> 4);
		hex_digest[2*i+1]= to_hex(digest_calculated[i] & 15);
	}

	cout << "sha digest check end" <<endl;



	cout << "regex test" <<endl;
	string path = "/connectV3?get_pre_connection_info=true&auth_key=gRI3tkBR9rZeuBUSoxnSBm7LvdCBCQoHYWJoaW5hdg%3D%3D";
	if(boost::regex_search(&path[0] , &path[path.length()-1] ,  boost::regex("^/connectV3"))){
		//goto REUSE_FD_HTTP_CALL_KEEP_ALIVE
		cout << " match " <<endl;
	}

	to_encrypt = "aslbslLILILILIASIUSDUI12312378";
	generate_auth_key(to_encrypt, a, l);

	char s3[500];
	int l3 = base64_encode(a, l , s3);
	cout << "base 64 encoded :"<< string(a, l) <<" : " << l <<  " : "<< string(s3, l3) << endl;

	char dest_2[500];
	size_t x=l3;
	x = base64_decode(s3, x, dest_2 );

	cout << "base 64 decoded :"<< string(dest_2, x) << " size "<< x << endl;
	char dest[100];
	l3 = urlencode(s3, l3, dest);

	cout << "auth_key + base64 encode + urlencoded : " << string(dest, l3) << endl;

	x = l3;
	urldecode_inplace(dest, x);
	cout << "url decode :"<< string(dest, x) << endl;
	x = base64_decode(dest, x, dest_2 );

	cout << "url decode + base 64 decode :"<< string(dest_2, x) << " size " << x <<endl;

	if(retrieve_auth(dest_2, x , auth_obj)){
		cout << auth_obj.node_id() <<endl;
	}
	else{
		cout << "its funcked !" <<endl;
	}


	for(int i=0;i<10;i++){
		unique_ptr<char, boost::function<void(void *)> > header_buff( (char *)mem_recycler.get(2048) ,  boost::bind(&MemRecycler::recyle, mem_recycler , boost::placeholders::_1, 2048) );
		header_buff[0] = 1;
	}

	map<string, int> m2;
	m2["a"] = 1;
	m2["b"] = 2;
	m2["c"] = 3;

	int *c1 = find_in_map<string, int>(m2, "a");

	if(c1!=NULL){
		std::cout<<  *c1 <<std::endl;
	}


	for(int i=0; i <100;i++){
		char arr[35];
		int l = gen_random(arr, 0);
		std:: cout << string(arr, l) << std::endl;
	}

	//push notification test
//send_a_push_notification("f6RJg8hJchU:APA91bFQ2MOAUVBO1AAwGB7aiGscdQTzEarQb3TDTdFtnJ1aykg-Su3GzgSqF0Ak4PeoAOWp_Dk6zH17nKZQFCnQuiBhoTDvvnboPYxgHLVt2JHognDA1N89vstIvX_47WCq2mkofMCj",  "feak", 0);


	test_convert_host_to_ip();




	return 0;
}

/*auth_key =
 *

gRI3tkBR9rZeuBUSoxnSBm7LvdCBCQoHYWJoaW5hdg==


gRJXq7bH3GwB4azf9zOM2IXBSgWBCAoGc2luZGh1
gRLxyoBs0BPhxVDqxlJOKz2nz5GBBwoFaGVsbG8=
gRILNJnAiwQQollQnFs+jb9e7oiBCAoGaGVsbG8y

*/
