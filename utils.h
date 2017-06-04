/*
 * utils.h
 *
 *  Created on: 22-Dec-2016
 *      Author: abhinav
 */

#ifndef UTILS_H_
#define UTILS_H_


#include <sys/time.h>
#include <map>

#include "utilities/json/json.hpp"
#include "boost/make_shared.hpp"


using std::map;


inline long int get_time_in_ms(){
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

int gen_random(char *s, int shard_id);

char* gen_random_string(char *s, const int len);

class StringPtr{
public:
	char * ptr = NULL;
	size_t len = 0;
	size_t buffer_size=0;



	StringPtr(){};
	StringPtr(char *ptr, size_t len):ptr(ptr) , len(len){};

};


struct StringPtrCompare
{
	//lhs < rhs
   bool operator() (const StringPtr& lhs, const StringPtr& rhs) const {
	   if(lhs.len != rhs.len)
		   return lhs.len < rhs.len;
       return strncasecmp(lhs.ptr , rhs.ptr , lhs.len)<0;
   }
};



template<typename A, typename B> B* find_in_map(map<A,B> &m , const A &key){
	typename map<A,B>::iterator it = m.find(key);
	if(it==m.end()){
		return NULL;
	}
	return &(it->second);
}



#endif
