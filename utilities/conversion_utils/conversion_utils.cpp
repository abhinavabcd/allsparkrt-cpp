/* 
   base64.cpp and base64.h

   Copyright (C) 2004-2008 René Nyffenegger

   This source code is provided 'as-is', without any express or implied
   warranty. In no event will the author be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this source code must not be misrepresented; you must not
      claim that you wrote the original source code. If you use this source code
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original source code.

   3. This notice may not be removed or altered from any source distribution.

   René Nyffenegger rene.nyffenegger@adp-gmbh.ch

*/

#include "../conversion_utils/conversion_utils.h"

#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

inline char from_hex(char ch) {
	  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

int itoa(unsigned long long int value, char *s, int base) {
	enum {
		kMaxDigits = 35
	};
	// check that the base if valid
	if (base < 2 || base > 16)
		return 0;
	int quotient = value;
	// Translating number to string with base:
	int c=0;
	do {
		s[c++] = "0123456789abcdef"[std::abs(quotient % base)];
		quotient /= base;
	} while (quotient);
	// Append the negative sign for base 10
//	if (value < 0 && base == 10)
//		s[c++]= '-';
	std::reverse(s, s+c);
	return c;
}
int atoi(const char *str, size_t max_len){
	int res =0;
    for (int i = 0; i<max_len; ++i)
        res = res*10 + str[i] - '0';
    return res;
}
size_t urlencode(char *src, size_t src_len, char *dest){
 char *initial = dest;
 for(int i=0;i<src_len;i++) {
	if (isalnum(*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~')
	  *dest++ = *src;
	else if (*src == ' ')
	  *dest++ = '+';
	else
	  *dest++ = '%', *dest++ = to_hex(*src >> 4), *dest++ = to_hex(*src & 15);
	src++;
  }
 return dest - initial;
}

void urldecode_inplace(char *src, size_t &length){
        char a, b;
        int write_ptr=0;
        int read_ptr=0;
        while (read_ptr<length) {
                if ((src[read_ptr] == '%') && (read_ptr+2)<length) {
                	    a = src[read_ptr+1];
                	    b = src[read_ptr+2];
                		src[write_ptr] = 16*from_hex(a)+from_hex(b);
                        write_ptr++;
                        read_ptr+=3;
                } else if (src[read_ptr] == '+') {
						src[write_ptr] = ' ';
						write_ptr++;
						read_ptr++;
                } else {
                	src[write_ptr] = src[read_ptr];
					write_ptr++;
					read_ptr++;
                }
        }
        length = write_ptr;
}

static const char *base64_valid_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(char const* in, unsigned int in_len, char *to_buff){


    int val=0, valb=-6;
    int out_len=0;
    for (int i=0;i<in_len;i++) {
    	unsigned char c = in[i];
        val = (val<<8) + c;
        valb += 8;
        while (valb>=0) {
            to_buff[out_len++] = base64_valid_chars[(val>>valb)&0x3F];
            valb-=6;
        }
    }
    if (valb>-6) to_buff[out_len++] = base64_valid_chars[((val<<8)>>(valb+8))&0x3F];
    while (out_len%4) to_buff[out_len++] = '=';
    return out_len;
}

int base64_decode(char *in , size_t in_len , char *to_buff){

    char T[256] = {-1};
    for (int i=0; i<64; i++) T[base64_valid_chars[i]] = i;
    int out_len=0;

    int val=0, valb=-8;
    for (int i=0;i<in_len;i++) {
    	unsigned char c = in[i];
        if (T[c] == -1) break;
        val = (val<<6) + T[c];
        valb += 6;
        if (valb>=0) {
        	to_buff[out_len++] = (val>>valb)&0xFF;
            valb-=8;
        }
    }
    while(out_len>0 && to_buff[out_len-1]==0){
    	out_len--;
    }
    return out_len;
}
/* Leaks memory on lookup failure, as it doesn't close underlying UDP socket */
bool convert_host_name_to_ip(const char *hostname , char* ipstr){
	int status;
	struct addrinfo hints, *p;
	struct addrinfo *servinfo;
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((status = getaddrinfo(hostname, NULL, &hints, &servinfo)) != 0) {
	    fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
	    return false;
	}
	bool ret = false;
	for (p=servinfo; p!=NULL; p=p->ai_next) {
	    struct in_addr  *addr;
	    if (p->ai_family == AF_INET) {
	        struct sockaddr_in *ipv = (struct sockaddr_in *)p->ai_addr;
	        addr = &(ipv->sin_addr);
	    }
	    else {
	        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
	        addr = (struct in_addr *) &(ipv6->sin6_addr);
	    }
	    strcpy(ipstr, inet_ntoa(*addr));
	    ret = true;
	}
	freeaddrinfo(servinfo);
	return ret;
}

