//mem.h

struct shmem {
  int ref_cnt;
  int owner;
  int not_published;
  uint region_size;
  char* shared_pages[NSHMPGS];
};
