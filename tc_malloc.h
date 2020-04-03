typedef struct Object* Object_T;
typedef struct Span* Span_T;
typedef struct ThreadCache* ThreadCache_T;

int spin_init();
pid_t gettid();
int get_length(int index);
int num_move_obj(size_t size);
void *tc_thread_init();


//returns pointer to span that contains the object
Span_T get_span_by_objaddr(void *object_addr);

//returns index in pagelist
int get_pagelist_index(void *pageaddress);

//returns index in centralfreelist or threadcache
int get_index_from_size (size_t size);
size_t get_size_from_index(int index);

//make a new span - take care of insufficient memory of page and span
Span_T init_span_allocator(int _pagenum);
Span_T span_allocator(int _pagenum);

//make a bunch of pages -> return the starting pointer of pages
void *page_allocator();

//initialize spanhash and pagelist
void *tc_central_init();

//divide span to objects - input the span we want to divide and the size of obj
int divide_span_to_object(Span_T cur_span, size_t obj_size);

int move_obj_from_centralfreelist_to_threadcache(int move_num, size_t sizeclass);

//spanhash index, move_num = MOVESPAN, size= size_object
//move span from spanheap to centralfreelist of amount INITIALSPANNUM
int move_span_from_spanhash_to_centralfreelist(int index, int move_num , size_t size, int init_or_not);

int tc_freelist_init();

void *tc_malloc(size_t size);

void tc_free(void *ptr);

void* small(int iter);

void* _malloc();


