#include <stdio.h>  // Include the standard I/O library

int fib(int n) {
  if(n < 2) return n;
  else return fib(n-1) + fib(n-2);
}

int main() {
  int val = fib(41);
  printf("%d\n", val);  // Correctly print the integer value
  return 0;
}