#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int initial_size, after_alloc, after_free;
  char *p;

  // Print initial memory size
  initial_size = memsize();
  printf("Initial memory size: %d bytes\n", initial_size);

  // Allocate 20k more bytes
  p = malloc(20480);
  if(p == 0) {
    printf("malloc failed\n");
    exit(1);
  }

  // Print memory size after allocation
  after_alloc = memsize();
  printf("Memory size after allocating 20k: %d bytes\n", after_alloc);

  // Free the allocated array
  free(p);

  // Print memory size after release
  after_free = memsize();
  printf("Memory size after freeing: %d bytes\n", after_free);

  exit(0);
}
