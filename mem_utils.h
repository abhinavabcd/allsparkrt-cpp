/*
 * mem_utils.h
 *
 *  Created on: Dec 19, 2016
 *      Author: abhinav
 */

#ifndef MEM_UTILS_H_
#define MEM_UTILS_H_

#include "boost/type_index.hpp"
#include "tbb/concurrent_queue.h"
#include "tbb/concurrent_hash_map.h"
#include "boost/shared_ptr.hpp"
#include "boost/bind/placeholders.hpp"
#include "boost/move/unique_ptr.hpp"
#include "boost/bind.hpp"
#include "server.h"
#include <stdlib.h>

using tbb::concurrent_queue;
using tbb::concurrent_hash_map;
using boost::shared_ptr;
using boost::movelib::unique_ptr;

class MemRecycler{
	concurrent_hash_map<size_t , concurrent_queue<void *>> mem_pool;
	concurrent_hash_map<size_t , concurrent_queue<void *>> mem_pool_by_type;
public:

	void* get(size_t size_of_memory);
	/*
	 * size in bytes
	 */
	void recyle(void *ptr , size_t size_of_memory);

	void free_all_pools();
	/*
	 *
	 */
	template <typename T> T* get(){
		void *obj;
		concurrent_hash_map<uintptr_t, concurrent_queue<void *>>::accessor mem_pool_accessor;
		size_t tid = T::ID;
		if(!mem_pool_by_type.find(mem_pool_accessor, tid )){
			mem_pool_by_type.insert(mem_pool_accessor, tid);
		}

		concurrent_queue<void*> &temp = mem_pool_accessor->second;
		while(!temp.try_pop(obj)){ //pool is empty , all ptrs are used up
			obj = new T();//allocate new memory
			temp.push(obj);
		}
		return obj;
	}


	template <typename T> void recycle(T *obj){
		concurrent_hash_map<size_t , concurrent_queue<void *>>::accessor mem_pool_accessor;
		size_t tid = T::ID;
		if(!mem_pool_by_type.find(mem_pool_accessor, tid)){
			delete obj;
			printf("should not happen should recyle correctly!");
			return;
		}
		obj->recycle();
		mem_pool_accessor->second.push(obj);
	}

	template <typename T> shared_ptr<T> get_shared(){
		T* obj = get<T>();
		return shared_ptr<T>(obj, boost::bind(&MemRecycler::recycle<T>, this,  boost::placeholders::_1));
	}

	template <typename T> unique_ptr<T> get_unique(){
		T* obj = get<T>();
		return unique_ptr<T>(obj, boost::bind(&MemRecycler::recycle<T>, this,  boost::placeholders::_1));
	}
};



#endif /* MEM_UTILS_H_ */
