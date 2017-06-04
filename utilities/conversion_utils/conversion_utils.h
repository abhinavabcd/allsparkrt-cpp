#include <string.h>

int base64_encode(char const* bytes_to_encode, unsigned int in_len, char *to_buff);
int base64_decode(char *encoded_string , size_t in_len , char *ret);


inline char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

int itoa(unsigned long long int value, char *s, int base);
int atoi(const char *str, size_t len);

void urldecode_inplace(char *src, size_t &length);
size_t urlencode(char *src, size_t src_len, char *dest);

bool convert_host_name_to_ip(const char *hostname , char* ipstr);
