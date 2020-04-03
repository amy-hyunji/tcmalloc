// TC_MALLOC.C //
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include "tc_malloc.h"

#define PAGELISTLEN 1048576*8 //2^20
#define PAGESIZE 4096
#define INITIALMOVESPAN 5
#define MOVESPAN 3 //spanheap에서 centralfreelist로 움직일 span 개수
#define SBRKSPAN 1024*1024 
#define SBRKPAGE 1024*1024*512 
#define THRESHOLD 1

int iter = 0;

struct Span {
    int obj_num;
    size_t size_class;
    Span_T next;
    Span_T prev;
    Object_T objects;
    void *pageaddress;
    int pagenum;
};

struct Object {
    Object_T next;
};

struct CentralFreeObject {
    Span_T empty;
    Span_T not_empty;
    int total_obj_num;
};

struct CentralFreeList {
    struct CentralFreeObject array[158];
};

struct SpanHash {
    Span_T array[256];
};

struct ThreadCache {
    Object_T array[158];
};

struct SpanHash spanhash;
struct CentralFreeList centralfreelist;
__thread struct ThreadCache threadcache;
__thread ThreadCache_T threadcachept;
Span_T pagelist[PAGELISTLEN] = {NULL};

Span_T curspanpt = NULL;
Span_T startspanpt = NULL;
Span_T endspanpt = NULL;

/*
for spinlock
*/
pthread_spinlock_t central_spinlock;
pthread_spinlock_t page_spinlock;

int spin_init(){
    pthread_spin_init(&central_spinlock, 0);
    pthread_spin_init(&page_spinlock, 0);
    return 0;
}


pid_t gettid(){
    return syscall(__NR_gettid);
}

/*
input: object address
output: pointer to span that contains the object
*/
Span_T get_span_by_objaddr(void *object_addr) {
    return pagelist[(uint32_t)((unsigned long)object_addr >> 12)]; //12
}

/*
input: pageaddress
output: index in pagelist
*/
int get_pagelist_index(void *pageaddress){
    return (uint32_t)((unsigned long)pageaddress>>12); //12
}

/*
input: size
output: index in centralfreelist or threadcache
*/
int get_index_from_size(size_t size){
    if (size <= 64) {
        int a = size/8;
        if (size%8 == 0) return a-1;
        else return a;
    }
    else if (size <= 2048) {
        int a = 8+(size-64)/64;
        if (size%64 == 0) return a-1;
        else return a;
    }
    else if (size < 8*PAGESIZE) {
        int a = 39+(size-2048)/256;
        if (size%256 == 0) return a-1;
        else return a;
    }
    else return 200; //large object
}

/*
input: index
output: size
*/
size_t get_size_from_index(int index){
    if (index <= 7) {return (1+index)*8;}
    else if (index <= 38) {return 64*(index-6);}
    else {return 256*(index-30);}
}

/*
    make a new span
    output: Span_T
*/
Span_T init_span_allocator(int _pagenum){

    //first initialization of span
    if (startspanpt == NULL){
      
        //first initializing the span
        startspanpt = (Span_T)sbrk(SBRKSPAN);
        curspanpt = startspanpt;
        endspanpt = (Span_T)sbrk(0);
        
        //curspanpt for spanhash.array[255]
        curspanpt->obj_num = 0;
        curspanpt->size_class = 0;
        curspanpt->prev = NULL;
        curspanpt->next = NULL;
        curspanpt->objects = NULL;
        curspanpt->pageaddress = NULL;
        curspanpt->pagenum = 0;
        curspanpt = curspanpt + 1;
        return curspanpt-1;
    }
}


Span_T span_allocator(int _pagenum){


    //allocate new span structure space
    if ((void *)endspanpt-(void *)curspanpt <= (int)(sizeof(struct Span))) {
        //need more space -> sbrk
        startspanpt = (Span_T)sbrk(SBRKSPAN);
        curspanpt = startspanpt;
        endspanpt = (Span_T)sbrk(0);
    }

    Span_T temp = curspanpt;
    temp->obj_num = 0;
    temp->size_class = 0;
    temp->prev = NULL;
    temp->next = NULL;
    temp->objects = NULL;
   
    int checkindex = _pagenum;
    while(1){
        if (get_length(checkindex) >= 1){
            int diff = spanhash.array[checkindex]->pagenum - _pagenum;
            Span_T checkspan = spanhash.array[checkindex]; //span I want to divide
            
            curspanpt += 1;

            ///////////////////////////
            Span_T newspan = curspanpt; //newspan of pagenumber diff
            newspan->obj_num = 0;
            newspan->size_class = 0;
            newspan->prev = NULL;
            Span_T _temp = spanhash.array[diff-1];
            newspan->next = _temp;
            if (_temp != NULL) _temp->prev = newspan;
            spanhash.array[diff-1] = newspan;
            newspan->pagenum = diff;
            newspan->pageaddress = checkspan->pageaddress;
            void *pgaddr = newspan->pageaddress;
            for (int i=0; i<diff; i++){
                pagelist[get_pagelist_index(pgaddr)] = newspan;
                pgaddr += PAGESIZE;
            }
            ///////////////////////////

            spanhash.array[checkindex] = checkspan->next; 

            checkspan->pagenum -= diff;
            checkspan->pageaddress += diff*PAGESIZE;
            Span_T temptemp = spanhash.array[_pagenum-1];
            spanhash.array[_pagenum-1] = checkspan;
            checkspan->next = temptemp;
            if (temptemp!=NULL) temptemp->prev = checkspan;

            
            curspanpt += 1;
            return spanhash.array[_pagenum-1];
        }
        if (checkindex == 254) break;
        checkindex += 1;
    }
   
    if (spanhash.array[255]->pagenum < _pagenum){

        void *newpage = page_allocator();
        spanhash.array[255]->pageaddress = newpage;
        spanhash.array[255]->pagenum = SBRKPAGE/PAGESIZE;
    }

    temp->pageaddress = spanhash.array[255]->pageaddress;
    temp->pagenum = _pagenum;
    spanhash.array[255]->pageaddress = spanhash.array[255]->pageaddress + _pagenum*PAGESIZE;

    spanhash.array[255]->pagenum = spanhash.array[255]->pagenum - _pagenum;
    
    void *startpage = temp->pageaddress;
    for (int i=0; i<_pagenum; i++){
        if (pagelist[get_pagelist_index(startpage)]!= NULL){
            Span_T temp = pagelist[get_pagelist_index(startpage)];
            exit(-1);
        }
        pagelist[get_pagelist_index(startpage)] = temp;
        startpage += PAGESIZE;
        
    }
    curspanpt = temp+1;
    return temp;
}


/*
   make a new bunch of pages
   return starting pointer of pages
*/
void* page_allocator(){
    void *returnval = (void *)sbrk(SBRKPAGE);
    return returnval;
}

/*
    initialize SpanHash and pagelist
    return starting point of page heap
    *****need to decide INITIALSPANNUM******
*/
void *tc_central_init() {
    spin_init();
    void *Head = sbrk(0);
    void *pagept = page_allocator(); //allocate pages
   
    pthread_spin_lock(&page_spinlock);
    spanhash.array[255] = init_span_allocator(0);
    spanhash.array[255]->pageaddress = pagept;
    spanhash.array[255]->pagenum = SBRKPAGE/PAGESIZE;
    pthread_spin_unlock(&page_spinlock);
    
    tc_freelist_init();
    return Head;
}

/*
    divide to objects
    input: object size
    input: span to divide
    OKAY
*/
int divide_span_to_object(Span_T cur_span, size_t obj_size){
    void *cur_page = cur_span->pageaddress;
    int cur_pagenum = cur_span->pagenum;
    cur_span->obj_num = cur_pagenum * PAGESIZE / obj_size;
    cur_span->size_class = obj_size;
    cur_span->objects = cur_page;
    Object_T curobj = cur_span->objects;
    for (int i=0; i<cur_span->obj_num; i++){
        cur_page += obj_size;
        curobj->next = cur_page;
        curobj = curobj->next;
    }
    Object_T prev = (void *)curobj-obj_size;
    prev->next = NULL;
    return cur_span->obj_num;
}

/*
   move objects from centralfreelist to threadcache
   OKAY
*/
int move_obj_from_centralfreelist_to_threadcache(int move_num, size_t size_class){
    pthread_spin_lock(&central_spinlock);
    int index = get_index_from_size(size_class);
    int temp_request_num = move_num;
    // move objects to threadcache
    while (temp_request_num != 0){
        
        if (centralfreelist.array[index].total_obj_num < move_num){
            int a = move_span_from_spanhash_to_centralfreelist(index, MOVESPAN, size_class, 1);
        }

        Object_T connection_need_obj = threadcache.array[index];
        Span_T check_span = centralfreelist.array[index].not_empty;
        Object_T startpt = check_span->objects;
        Object_T nextpt = startpt;
        if (check_span->obj_num > temp_request_num){
            while (temp_request_num != 0){
                if (temp_request_num!=1) nextpt = nextpt->next;
                temp_request_num -= 1;
                check_span->obj_num -=1;
                centralfreelist.array[index].total_obj_num -= 1;
            }
            threadcache.array[index] = startpt;
            check_span->objects = nextpt->next;
            nextpt->next = connection_need_obj;
        }
        
        else if (check_span->obj_num <= temp_request_num){
            temp_request_num -= check_span->obj_num;
            centralfreelist.array[index].total_obj_num -= check_span->obj_num;
            check_span->obj_num = 0;
           

            threadcache.array[index] = startpt;
            check_span->objects = NULL;

            //move check_span to empty
            centralfreelist.array[index].not_empty = check_span->next;
            Span_T temp = centralfreelist.array[index].empty;
            centralfreelist.array[index].empty = check_span;
            check_span->next = temp;
            check_span->prev = NULL;
            if (temp != NULL) temp->prev = check_span;
        }
        else {
            printf("something wrong in object allocator\n");
            exit(-1);
        }
    }
    pthread_spin_unlock(&central_spinlock);

    return 1;
}

int get_length(int index){
    int _return = 0;
    Span_T temp = spanhash.array[index];
    while (temp != NULL) {
        _return += 1;
        temp = temp->next;
    }
    return _return;
}


/*
   move span from spanhash to centralfreelist: INITIALSPANNUM만큼
   if not sufficient - span allocator
    index -> 가져올 span이 spanhash에 위치한 index
    move_num -> span 몇 개 움직일 지 
    size -> 몇 size object로 분해할지
    init_or_not 1: not init ->spinlock 없애기
*/

int move_span_from_spanhash_to_centralfreelist(int index, int move_num, size_t size, int init_or_not){
    pthread_spin_lock(&page_spinlock);
    Span_T startspan = spanhash.array[index];
    int length = get_length(index);
    int temp_move_num = move_num; 

    while (length < temp_move_num){
        //need to allocate more span
        Span_T _prev = spanhash.array[index];
        Span_T new_span = NULL;
        for (int i=0; i<MOVESPAN; i++){
            new_span = span_allocator(index+1); 
            spanhash.array[index] = new_span;
            new_span->next = _prev;
            if (_prev != NULL) _prev->prev = new_span;
            _prev = new_span;
        }
        length += MOVESPAN;
    }

    //sufficient span exists
    Span_T span_start = spanhash.array[index];
    Span_T span_end = span_start;
    for (int i=0; i<MOVESPAN; i++){
        centralfreelist.array[index].total_obj_num += divide_span_to_object(span_end, size);
        if (i!=move_num-1) span_end = span_end->next;
    }
    
    spanhash.array[index] = span_end->next;
    if (spanhash.array[index]!=NULL) spanhash.array[index] -> prev = NULL;
    pthread_spin_unlock(&page_spinlock);

    if (init_or_not == 0) pthread_spin_lock(&central_spinlock);
    Span_T central_start = centralfreelist.array[index].not_empty;
    centralfreelist.array[index].not_empty = span_start;
    span_end->next = central_start;
    span_start->prev = NULL;
    if (central_start!= NULL){central_start->prev = span_end;}
    if (init_or_not == 0) pthread_spin_unlock(&central_spinlock);
   
    return 1;
}

/*
   initialize centralfreelist
*/
int tc_freelist_init() {
    //freelist에 충분한 양의 object가 없는 경우(MOVEOBJECT보다 적은 경우)
    //pageheap에서 가져오기
    for (int i=0; i<158; i++){ //158
        int a = move_span_from_spanhash_to_centralfreelist(i, INITIALMOVESPAN, get_size_from_index(i), 0);
    }

    return 1;
}

int num_move_obj(size_t size){
    int threshold = 8*PAGESIZE;
    int compare = threshold/size;
    if (compare>=THRESHOLD) return THRESHOLD;
    else return compare;
}

void *tc_thread_init(){
    for (int i=0; i<158; i++){ //158
        size_t size = get_size_from_index(i);
        move_obj_from_centralfreelist_to_threadcache(num_move_obj(size), size);
    }
   
    return (void *)threadcachept; 
}

/*
   initialize threadcache for each thread 
   *******need to decide number of objects to get from threadcache*******
*/
void *tc_malloc(size_t size){
    if (size < PAGESIZE*8){
        //small object malloc
        int index = get_index_from_size(size);
        
        if (threadcache.array[index] == NULL){
            //allocate objects from centralfreelist
            move_obj_from_centralfreelist_to_threadcache(num_move_obj(size), size);
            int a=0;
            Object_T temp = threadcache.array[index];
            while (temp != NULL){
                a += 1;
                temp = temp->next;
            }
        }
        //sufficient objects exist
        Object_T return_obj = threadcache.array[index];
        threadcache.array[index] = return_obj->next;
        return return_obj;
    }
    else {
        //large object malloc
        pthread_spin_lock(&page_spinlock);
        int need_pagenum = 1+(size/PAGESIZE);
        if (size%PAGESIZE == 0) need_pagenum -=1;
        int return_index = need_pagenum - 1;
        if (spanhash.array[return_index]==NULL){
            //need to allocate new span
            Span_T prev_connection = spanhash.array[return_index];
            Span_T new_span = NULL;
            for (int i=0; i<MOVESPAN; i++){
                new_span = span_allocator(need_pagenum); //spna
                new_span->next = prev_connection;
                if (prev_connection!= NULL) prev_connection->prev = new_span;
                spanhash.array[return_index] = new_span;
                prev_connection = new_span;
            }
        }
        //sufficient span exist
        Span_T return_span = spanhash.array[return_index];
        spanhash.array[return_index] = return_span->next;
        if (spanhash.array[return_index]!= NULL) spanhash.array[return_index]->prev = NULL;
        pthread_spin_unlock(&page_spinlock);
        return return_span->pageaddress;
        
    }
}

void tc_free(void *ptr){
    Span_T check_span = get_span_by_objaddr(ptr);
    if (check_span->size_class != 0){
        //small object
        int index = get_index_from_size(check_span->size_class);
        Object_T cur = ptr;
        Object_T conn = threadcache.array[index];
        threadcache.array[index] = cur;
        cur->next = conn;
        
    }
    else {
        //large object - need to revise
        pthread_spin_lock(&page_spinlock);
        void *check_addr = check_span->pageaddress;
        int prev = get_pagelist_index(check_addr-PAGESIZE);
        int next = get_pagelist_index(check_addr+check_span->pagenum);
        Span_T prev_span = spanhash.array[check_span->pagenum-1];
        spanhash.array[check_span->pagenum-1] = check_span;
        check_span->next = prev_span;
        if (prev_span != NULL) prev_span->prev = check_span;
        check_span->prev = NULL;
        pthread_spin_unlock(&page_spinlock);
    }
}

/*
void* small(int iter){
   
    void *returnval = NULL;
    if (iter < 10) {
        char *return8 = tc_malloc(8);
        char *return128 = tc_malloc(128);
        char *return1024 = tc_malloc(1024);
        
        memset(return1024, 'a', 512);
        memset(return1024+512, 'b', 512);
        for (int i=0; i<1024; i++){
        }
        

        char *large = tc_malloc(PAGESIZE*50);
        tc_free(return8);
        tc_free(return128);
        tc_free(return1024);
        tc_free(large);
    }
    else {
        char *return8 = tc_malloc(8);
        char *return128 = tc_malloc(128);
        char *return1024 = tc_malloc(1024);
        tc_free(return8);
        tc_free(return128);
        tc_free(return1024);
    }
    
    return returnval;
}

void* _malloc(){
    tc_thread_init();
    iter += 1;
    void *addr = NULL;
     
    for (int i=0; i<100; i++) {
       addr =  small(i);
    }
    
    return addr;
}

int main(){

    pthread_t thr[1000];
    struct timeval tv1, tv2;

    gettimeofday(&tv1, NULL);
    tc_central_init();
    
    for (int i=0; i<1000; i++){
        pthread_create(&thr[i], NULL, _malloc, NULL);
    }

    for (int i=0; i<1000; i++){
        pthread_join(thr[i], NULL);
    }
    gettimeofday(&tv2, NULL);

    if (tv1.tv_usec > tv2.tv_usec){
        tv2.tv_sec--;
        tv2.tv_usec += 1000000;
    }
    printf("Result - %ld.%ld\n", tv2.tv_sec-tv1.tv_sec, tv2.tv_usec-tv1.tv_usec);
    
    return 1;
}
*/
