#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"
#include <stdbool.h>
/* This is the wait queue structure */
struct wait_queue {
    Tid id;
    struct wait_queue* next;
};

enum status{
    RUN = -1,
    READY = -2,
    EXITED = -3,
    KILLED = -4,
    INIT = -5,
    SLEEP = -6
};

/* This is the thread control block */
struct thread {
    enum status st;
    ucontext_t context;
    void *sp;
    Tid tid;
    struct wait_queue *wq;
    
};

struct ready_queue{
    Tid id;
    struct ready_queue* next;
};



volatile static Tid current_t;
volatile static Tid num_t;

static struct thread tr_array[THREAD_MAX_THREADS];
static struct ready_queue *rq;

void queue_push(Tid id, struct ready_queue *rq );
Tid queue_pop(Tid id, struct ready_queue *rq);
Tid next_thread();


void queue_push_wq(Tid id, struct wait_queue *wq ){
    int enabled = interrupts_set(0);
    if (wq == NULL){  
        interrupts_set(enabled);
        return;
    }
    struct wait_queue *orig = wq;
    struct wait_queue *new;
    
    new = (struct wait_queue*)malloc(sizeof(struct wait_queue));
    new->id = id;
    new->next = NULL;
    
    if (wq->next == NULL){
        wq->next = new;
        interrupts_set(enabled);
        return;
    }
    else{
        while (orig->next != NULL){
            orig = orig->next;
        }
        orig->next = new;
        interrupts_set(enabled);
        return;
    }  
    
}

Tid queue_pop_wq(Tid id, struct wait_queue *wq){
    int enabled = interrupts_set(0);
    struct wait_queue *curr = wq->next;
    if (curr == NULL){
        return THREAD_NONE;
    }
    wq->next = curr->next;
    Tid ret = curr->id;
    free(curr);
    interrupts_set(enabled);
    return ret;
}


void queue_push(Tid id, struct ready_queue *rq ){
    int enabled = interrupts_set(0);
    if (rq == NULL){  
        interrupts_set(enabled);
        return;
    }
    struct ready_queue *orig = rq;
    struct ready_queue *new;
    
    new = (struct ready_queue*)malloc(sizeof(struct ready_queue));
    new->id = id;
    new->next = NULL;
    
    if (rq->next == NULL){
        rq->next = new;
        interrupts_set(enabled);
        return;
    }
    else{
        while (orig->next != NULL){
            orig = orig->next;
        }
        orig->next = new;
        interrupts_set(enabled);
        return;
    }  
    
}

Tid queue_pop(Tid id, struct ready_queue *rq){
    int enabled = interrupts_set(0);
    if (id == THREAD_SELF){
        id = current_t;
    }
    else if (id == THREAD_ANY){
        if ((tr_array[current_t].st == RUN) && num_t == 1){
            interrupts_set(enabled);
            return THREAD_NONE;
        }
         
        else{
            id = rq->next->id;
        }
    }
    struct ready_queue *curr = rq->next;
    struct ready_queue *prev = NULL;

    while (curr != NULL || curr->id == id  ){
        if (curr->id == id){
            Tid ret = curr->id;
            if (prev == NULL){
                rq->next = curr->next;
            }
            else{
                prev->next = curr->next;
            }      
            free(curr);
            interrupts_set(enabled);
            return ret;
        }
        prev = curr;
        curr = curr->next;
    }  
    interrupts_set(enabled);
    return THREAD_INVALID;
}

Tid next_thread(){
    int enabled = interrupts_set(0);
    for (unsigned i = 0; i < THREAD_MAX_THREADS; i++){
        if (tr_array[i].st == INIT || tr_array[i].st == EXITED){
            interrupts_set(enabled);
            return i;
        }
    }
    interrupts_set(enabled);
    return -1;
}

void
thread_init(void){
    num_t = 1;
    current_t = 0;
    for (int i = 0; i < THREAD_MAX_THREADS; i++){
        tr_array[i].st = INIT;
    }
    tr_array[current_t].st = RUN;
    tr_array[current_t].wq = wait_queue_create();
    getcontext(&tr_array[current_t].context);
  
    rq = (struct ready_queue*)malloc(sizeof(struct ready_queue));
    rq->next = NULL;
    
}

Tid
thread_id(){
    return current_t;
}

void
thread_stub(void (*thread_main)(void *), void *arg)
{   
    interrupts_on();
    thread_main(arg);
    thread_exit(current_t);     
    exit(0);
}

Tid
thread_create(void (*fn) (void *), void *parg)
{       int enabled = interrupts_set(0);
	if (num_t >= THREAD_MAX_THREADS) {
            interrupts_set(enabled);
            return THREAD_NOMORE;
	}
        
        void *sp = malloc(THREAD_MIN_STACK);
	if(sp == NULL){ 
            interrupts_set(enabled);
            return THREAD_NOMEMORY;
        }

        Tid id = next_thread();
        assert(id != -1);
            
	tr_array[id].sp = sp;
	tr_array[id].st = READY;
        tr_array[id].tid = id;
        tr_array[id].wq = wait_queue_create();
        
        getcontext(&(tr_array[id].context));
        tr_array[id].context.uc_mcontext.gregs[REG_RDI] = (unsigned long)fn;
        tr_array[id].context.uc_mcontext.gregs[REG_RSI] = (unsigned long)parg;
        tr_array[id].context.uc_mcontext.gregs[REG_RIP] = (unsigned long)&thread_stub;
        tr_array[id].context.uc_mcontext.gregs[REG_RSP] = (unsigned long)(sp  - (unsigned long)(sp)%16 + THREAD_MIN_STACK - 8); 
        
        num_t+=1;
        
        queue_push(id, rq);
        
        struct ready_queue *head = rq->next;
        
        while (head->next != NULL){
            head = head->next;
        }
        
        interrupts_set(enabled);
          
	return id;
}

Tid
thread_yield(Tid want_tid)
{
    int enabled = interrupts_set(0);
    bool popped = false; 
    
    if (tr_array[thread_id()].st != EXITED){
        for (int i = 0; i < THREAD_MAX_THREADS; i++){
            if (i != current_t && tr_array[i].st == EXITED){
                tr_array[i].st = INIT;
                free(tr_array[i].sp);
                wait_queue_destroy(tr_array[i].wq);
                tr_array[i].wq = NULL;
            }
        }
    }
  
    
    if (want_tid == THREAD_SELF || want_tid == current_t){
        interrupts_set(enabled);
        return current_t;
    }
    else if (want_tid == THREAD_ANY){
        if (num_t == 1 && tr_array[current_t].st == RUN){
            interrupts_set(enabled);
            return THREAD_NONE;
        }
        else{
            int a = queue_pop(THREAD_ANY, rq);
            want_tid = a; 
            popped = true;
        }
    }     
        
    if (!(want_tid >=0 && want_tid < THREAD_MAX_THREADS && (tr_array[want_tid].st==READY || (tr_array[want_tid].st==RUN)||
												(tr_array[want_tid].st==KILLED)))){
        interrupts_set(enabled);
        return THREAD_INVALID;
    }
    
    int d = 0;
    struct ucontext_t ucp;
    getcontext(&ucp);
    
    if (d){
        d = 0;
        interrupts_set(enabled);
        return want_tid;
    }  
    else if (d == 0){
        d = 1;
        if (!popped ){
            queue_pop(want_tid, rq);
        }
        if (tr_array[current_t].st == RUN){
            tr_array[current_t].st = READY;
            queue_push(current_t,rq);
        }
        tr_array[current_t].context = ucp;

        if(tr_array[want_tid].st != KILLED){
            tr_array[want_tid].st = RUN;
        }
        current_t = want_tid;
        setcontext(&(tr_array[want_tid].context));
    }
    interrupts_set(enabled);
    return want_tid;

}

Tid
thread_kill(Tid tid)
{
    int enabled = interrupts_set(0);
    
    if (!(tid >= 0 && tid < THREAD_MAX_THREADS && tid != current_t)){
        interrupts_set(enabled);
        return THREAD_INVALID;
    } 
    
    if (tr_array[tid].st != READY && tr_array[tid].st !=SLEEP){
        interrupts_set(enabled);
        return THREAD_INVALID;
    }
    
    tr_array[tid].st = EXITED;
    queue_pop(tid, rq);
    num_t-=1;
    
    interrupts_set(enabled);
    return tid;
}

volatile static int exitcode = 0;
bool exited = false;
//bool waited = true;

void
thread_exit(int exit_code)
{
    int enabled = interrupts_set(0);
    if (num_t == 1 && tr_array[current_t].wq->next == NULL){
        exit(0);
    }
    
    tr_array[current_t].st = EXITED;
    num_t-=1;
    thread_wakeup(tr_array[current_t].wq,1);
    
    exitcode = exit_code;
    exited = true;
    

    
//    if (exited){
//        exitcode = exit_code;
//    }
//    else{
//        exitcode = 0;
//    }
    
    interrupts_set(enabled);
    thread_yield(THREAD_ANY);
}


/*******************************************************************
 * Important: The rest of the code should be implemented in Lab 3. *
 *******************************************************************/

/* make sure to fill the wait_queue structure defined above */
struct wait_queue *
wait_queue_create()
{
    struct wait_queue *wq;
    wq = malloc(sizeof(struct wait_queue));
    assert(wq);
    wq->next = NULL;

    return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
    struct wait_queue *curr = wq->next;
    struct wait_queue *next = NULL;
    while (curr != NULL){
        next = curr->next;
        free(curr);
        curr = next;
    }       
    free(wq);
}

Tid
thread_sleep(struct wait_queue *wq)
{   
    int enabled = interrupts_set(0);
    if (wq == NULL) {
        interrupts_set(enabled);
        return THREAD_INVALID;
    }
    if (num_t == 1) {
        interrupts_set(enabled);
        return THREAD_NONE;
    }
    else {
        tr_array[current_t].st = SLEEP;
        queue_push_wq(current_t, wq);
        num_t-=1;
        interrupts_set(enabled);
        return thread_yield(THREAD_ANY);
    }
}

int
thread_wakeup(struct wait_queue *wq, int all)
{
    int enabled = interrupts_set(0);
    if (wq == NULL) {
        interrupts_set(enabled);
        return 0;
    }
    if (all){
        int num = 0;
        while (thread_wakeup(wq,0) != 0){
            num+=1;
        }
        interrupts_set(enabled);
        return num;
    }
    else{
        Tid ret = queue_pop_wq(THREAD_ANY, wq);
        if (ret != THREAD_NONE){
            tr_array[ret].st = READY;        
            queue_push(ret,rq);
            num_t+=1;     
            interrupts_set(enabled);
            return 1;
        }
        else {
            interrupts_set(enabled);
            return 0;
        }
    }
}

Tid
thread_wait(Tid tid, int *exit_code)
{   
    int enabled = interrupts_set(0);
    if ((tid == current_t || tid < 1 || tid > THREAD_MAX_THREADS || tr_array[tid].st == INIT || tr_array[tid].st == EXITED)){
        interrupts_set(enabled);
        return THREAD_INVALID;
    }
    Tid ret = thread_sleep(tr_array[tid].wq);
    if (ret == THREAD_NONE){
        exit(0);
    }
    exited = false;
    if (exit_code != NULL){ 
        *exit_code = exitcode;
    }
    interrupts_set(enabled);
    
//    if (exitcode != 0 && exit_code != NULL){
//        return *exit_code;
//    }
//    else{
    return tid;
    //}
}

struct lock {
    int lock_st;
    Tid id;
    struct wait_queue *lock_wq;
};

struct lock *
lock_create()
{
    int enabled = interrupts_set(0);
    struct lock *lock;

    lock = malloc(sizeof(struct lock));
    assert(lock);

    lock->lock_st = 0;
    lock->lock_wq = wait_queue_create();
    interrupts_set(enabled);
    return lock;
}

void
lock_destroy(struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(lock != NULL);

    wait_queue_destroy(lock->lock_wq);
    interrupts_set(enabled);
    free(lock);
}

void
lock_acquire(struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(lock != NULL);

    while(lock->lock_st==1){
        thread_sleep(lock->lock_wq);
    }
    lock->lock_st=1;
    lock->id = current_t;

    interrupts_set(enabled);
}

void
lock_release(struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(lock != NULL);
    lock->lock_st = 0;
    thread_wakeup(lock->lock_wq, 1);
    interrupts_set(enabled);
}

struct cv {
    struct wait_queue *cv_wq;
};

struct cv *
cv_create()
{
    int enabled = interrupts_set(0);
    struct cv *cv;

    cv = malloc(sizeof(struct cv));
    assert(cv);
    cv->cv_wq = wait_queue_create();
 
    interrupts_set(enabled);
    return cv;
}

void
cv_destroy(struct cv *cv)
{   
    int enabled = interrupts_set(0);
    assert(cv != NULL);
    wait_queue_destroy(cv->cv_wq);
    free(cv);
    interrupts_set(enabled);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(cv != NULL);
    assert(lock != NULL);
    if (lock->id == current_t){
        int enabled = interrupts_set(0);
        lock_release(lock);
        thread_sleep(cv->cv_wq);
        interrupts_set(enabled);
        lock_acquire(lock);
    }
    interrupts_set(enabled);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(cv != NULL);
    assert(lock != NULL);
    if (lock->id == current_t){
        thread_wakeup(cv->cv_wq, 0);
    }
    interrupts_set(enabled);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
    int enabled = interrupts_set(0);
    assert(cv != NULL);
    assert(lock != NULL);
    if (lock->id == current_t){
        thread_wakeup(cv->cv_wq, 1);
    }
    interrupts_set(enabled);
}
