/*
 * mem_utils.cpp
 *
 *  Created on: 26-Dec-2016
 *      Author: abhinav
 */


#include "mem_utils.h"


void* MemRecycler::get(size_t size_of_memory){
	void * ret = NULL;

	concurrent_hash_map<size_t , concurrent_queue<void *>>::accessor mem_pool_accessor;
	if(!mem_pool.find(mem_pool_accessor, size_of_memory)){
		mem_pool.insert(mem_pool_accessor, size_of_memory);
	}
	while(!mem_pool_accessor->second.try_pop(ret)){//pool is empty , all ptrs are used up
		ret = malloc(size_of_memory);//allocate new memory
		mem_pool_accessor->second.push(ret);
	}
	return ret;
}

/*
 * size in bytes
 */
void MemRecycler::recyle(void *ptr , size_t size_of_memory){
	concurrent_hash_map<size_t , concurrent_queue<void *>>::accessor mem_pool_accessor;
	if(!mem_pool.find(mem_pool_accessor, size_of_memory)){
		free(ptr);
		printf("should not happen %d \n", size_of_memory);
		return;
	}
	mem_pool_accessor->second.push(ptr);
}



void MemRecycler::free_all_pools(){
	int c = 0;
	for(concurrent_hash_map<size_t , concurrent_queue<void *>>::iterator it = mem_pool_by_type.begin();it!= mem_pool_by_type.end() ;it++){
		void *ptr = NULL;
		while(it->second.try_pop(ptr)){
			if(it->first==2)
				delete (Connection*)ptr;

			if(it->first==1)
				delete (WrappedMessage*)ptr;

			if(it->first==3)
				delete (MessageTransitToThread*)ptr;

			c++;
		}
	}
	std::cout << "freeing " << c << " ptr allocations from pool" <<std::endl;
	mem_pool_by_type.clear();


	c = 0;
	for(concurrent_hash_map<size_t , concurrent_queue<void *>>::iterator it = mem_pool.begin();it!= mem_pool.end();it++){
		void *ptr = NULL;
		while(it->second.try_pop(ptr)){
				free(ptr);
				c++;
		}
	}
	std::cout << "freeing " << c << " mem allocations from pool" <<std::endl;
	mem_pool.clear();



}
