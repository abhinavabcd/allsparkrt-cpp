CC = g++ -std=c++11 -g -Wall -fPIC -fpermissive -DUSE_TORNADO_AUTH_KEY 

OUTFILENAME = server


VPATH=wire_messages
	
PICOHTTPPARSER_DIR = picohttpparser/

PROTOBUF_MESSAGES = wire_messages/

EXTERNAL_INCLUDES = -I../protobuf/src -I../tbb/include -I../boost/ -I../mysql-connector-c++/include/ -I../openssl/include -I./spdlog/include


EXTERNAL_LIBS = -L../boost/stage/lib/ -L../protobuf/src/.libs/ -L../openssl/ -L../mysql-connector-c++/lib/ -L./libco/lib  -L./shared_libs/ #-L../tbb2017_20161128oss/lib/intel64/gcc4.7/ -L../mysql-connector-c++-1.1.8/lib/

EXTERNAL_RPATH = -Wl,-rpath,../boost/stage/lib/ -Wl,-rpath,../protobuf/src/.libs/ -Wl,-rpath,../openssl/ -Wl,-rpath,./shared_libs/ #-Wl,-rpath,../mysql-connector-c++-1.1.8/lib/ -Wl,-rpath,../tbb2017_20161128oss/lib/intel64/gcc4.7/ -Wl,-rpath,./libco/solib

#default
debug : server_without_libs
	g++ -o $(OUTFILENAME) server_without_libs.o $(EXTERNAL_RPATH) $(EXTERNAL_LIBS) -l:libboost_regex.a  -l:libprotobuf.a -l:libboost_system.a -l:libboost_thread.a -l:libssl.a -l:libcrypto.a -l:libmysqlcppconn-static.a -lcolib -ltbb -ldl  -lpthread


#release
release : CC := $(CC)
release : OUTFILENAME := server-no-ssl
release : debug

#release-ssl
release-ssl : CC := $(CC) -DSSL_SERVER
release-ssl : OUTFILENAME := server-ssl
release-ssl : debug



	
server_without_libs : server.o mysql_db.o auth_utils.o http_and_websocket_utils.o http_calls.o ssl_utils.o utils.o mem_utils.o config.o wire_messages utilities  make_libco picohttpparser tasks
	ld -o server_without_libs.o -r config.o server.o mysql_db.o auth_utils.o http_and_websocket_utils.o http_calls.o ssl_utils.o utils.o mem_utils.o wire_messages/*.o utilities/conversion_utils/*.o utilities/HMAC_SHA1/*.o picohttpparser/picohttpparser.o tasks/connection_creator_task.o tasks/flush_messages_to_db.o tasks/writer_task.o
	
test : test.o server_without_libs
	ld -o test_server.o -r test.o server_without_libs.o
	g++ -o test test_server.o $(EXTERNAL_RPATH) $(EXTERNAL_LIBS) -lboost_regex  -lmysqlcppconn-static  -l:libprotobuf.a -lboost_system -lboost_thread -lcolib -ltbb -lpthread -ldl -lssl -lcrypto
	

utilities/conversion_utils/%.o: utilities/conversion_utils/%.cpp
	-$(CC) $(EXTERNAL_INCLUDES) -c -o $@ $< $(CFLAGS)
 
utilities/HMAC_SHA1/%.o: utilities/HMAC_SHA1/%.cpp
	-$(CC) $(EXTERNAL_INCLUDES) -c -o $@ $< $(CFLAGS)

utilities/string_hash/%.o: utilities/string_hash/%.cpp
	-$(CC) $(EXTERNAL_INCLUDES) -c -o $@ $< $(CFLAGS)
 
 
  
utilities: utilities/conversion_utils/conversion_utils.o utilities/HMAC_SHA1/SHA1.o utilities/HMAC_SHA1/HMAC_SHA1.o utilities/string_hash/murmurhash3.o


tasks: tasks/flush_messages_to_db.o tasks/connection_creator_task.o tasks/writer_task.o


tasks/%.o : tasks/%.cpp
	-$(CC) $(EXTERNAL_INCLUDES) -c $< -o $@ $(CFLAGS)


picohttpparser: picohttpparser/picohttpparser.o

picohttpparser/%.o : picohttpparser/%.c
	-$(CC) $(EXTERNAL_INCLUDES) -c $< -o $@ $(CFLAGS)


wire_messages : wire_messages/wire_transfers.pb.o

wire_messages/%.o : wire_messages/%.cc
	-$(CC) $(EXTERNAL_INCLUDES) -c $< -o $@ $(CFLAGS)



%.o: %.c
	-$(CC) $(EXTERNAL_INCLUDES) -c -o $@ $< $(CFLAGS)

%.o: %.cpp
	-$(CC) $(EXTERNAL_INCLUDES) -c -o $@ $< $(CFLAGS)


make_libco:
	$(MAKE) -C ./libco/ all

	


clean:
	find . -type f -not -path ./libco \( -name '*.o' -o -name 'server' \) -delete
	$(MAKE) -C ./libco/ clean


