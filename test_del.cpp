#include <iostream>
#include<string.h>
struct S {
  S() { std::cout << "S::S()\n"; }
  ~S() { std::cout << "S::~S()\n"; }
};

int main () {
  int i = 10;
  char *s = "hello";
  std::cout << strlen(s) << s << std::endl;
}
