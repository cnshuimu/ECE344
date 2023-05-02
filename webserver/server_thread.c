#include "request.h"
#include "server_thread.h"
#include "common.h"
#include <pthread.h>
#include <stdbool.h>

pthread_mutex_t lock;
//#define HASH_SIZE 2000000
int HASH_SIZE = 0;

struct server {
	int nr_threads; 
	int max_requests; 
	int max_cache_size; 
	int exiting;

        int *conn_buf; 
        pthread_t *threads;
        int request_head;
        int request_tail;
        pthread_mutex_t mutex;
	pthread_cond_t prod_cond;
	pthread_cond_t cons_cond;
        
	struct cache *web_cache;
};

struct queue {
    int hash_value;
    struct queue* next;
};

struct file {
	char *name;
	struct file_data *data;
};

struct cache {
	int curr_size;
	int max_size;
	int hashtable_size;
	struct queue *LRU;
	struct file **hashtable; 
};

/* initialize file data */
static struct file_data *
file_data_init(void) 
{
	struct file_data *data;

	data = Malloc(sizeof(struct file_data));
	data->file_name = NULL;
	data->file_buf = NULL;
	data->file_size = 0;
	return data;
}

/* free all file data */
static void 
file_data_free(struct file_data *data)
{
	free(data->file_name);
	free(data->file_buf);
	free(data);
}

static void *do_server_thread(void *arg);

struct file *cache_lookup(struct server *sv, char *name);
bool cache_insert(struct server *sv, struct file_data *data);
bool cache_evict(struct server *sv, int file_size);

unsigned long hashing(struct server *sv, char *string);
void queue_pop(struct queue *LRU, int idx);
void LRU_update(struct queue *LRU, int idx);
void LRU_replace(struct queue *LRU, int idx); 

void queue_pop(struct queue* LRU, int idx) {
    struct queue* curr;
    struct queue* prev; 

    if(LRU == NULL || LRU->next == NULL){
 	return; 			
    }
    prev = LRU;
    curr = LRU->next;
    
    if (prev->hash_value == idx){
        LRU = LRU->next;
        free(prev);
        return;
    }
    
    while (curr->hash_value != idx && curr->next != NULL) {
        prev = curr;
        curr = curr->next;
    }

    if(curr->hash_value == idx) {
        prev->next = curr->next;
        free(curr);
        curr = NULL;
        return;
    }
}

unsigned long hashing(struct server *sv, char *string) {
    unsigned long hash = 5381;
    int c;
    
    while ((c = *string++) != '\0')
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash % HASH_SIZE;
}

struct file *cache_lookup(struct server *sv, char *name){
    struct file *target;
    int i = hashing(sv, name); 
        
    if (sv->web_cache->hashtable == NULL){
        return NULL;
    }
    while (sv->web_cache->hashtable[i] != NULL){
        target = sv->web_cache->hashtable[i];
        if (strcmp(target->name, name) == 0){
            return target;
        } 
        i+=1;
        i = i % HASH_SIZE;
    }

    return NULL;
}

bool cache_insert(struct server *sv, struct file_data *data){
    if (cache_lookup(sv, data->file_name) != NULL){
        return true; 
    }
    else if (cache_evict(sv, data->file_size)){ 
        struct file *new_file;
        unsigned long i = hashing(sv, data->file_name);

        if (sv->web_cache->hashtable == NULL){
            return false;
        }
        while(sv->web_cache->hashtable[i] != NULL){
            if (strcmp(sv->web_cache->hashtable[i]->name, data->file_name) == 0){
                break;
            }
            i += 1;
            i = i % HASH_SIZE;
        }
        new_file = (struct file*)malloc(sizeof(struct file));

        new_file->name = (char *)malloc(sizeof(char) * (strlen(data->file_name) + 1));
        strcpy(new_file->name, data->file_name);
        new_file->name[strlen(data->file_name)] = '\0';
        new_file->data = data;
        sv->web_cache->hashtable[i] = new_file;
        sv->web_cache->curr_size += data->file_size;
        LRU_update(sv->web_cache->LRU, i);
        return true;  
    }
    return false;
}

bool cache_evict(struct server *sv, int file_size){
    if (file_size <= sv->web_cache->max_size - sv->web_cache->curr_size){
        return true; 
    }
    else if (file_size > sv->web_cache->max_size){
        return false; 
    }
    else{
        
        struct cache *cache = sv->web_cache;
        struct queue *curr = cache->LRU;

        while(curr != NULL && (cache->max_size - cache->curr_size) < file_size) {
            struct file *file = cache->hashtable[curr->hash_value];
            if(file != NULL){
                cache->curr_size -= file->data->file_size;
                file_data_free(cache->hashtable[curr->hash_value]->data);
                free(cache->hashtable[curr->hash_value]->name);
                cache->hashtable[curr->hash_value] = NULL;
                queue_pop(cache->LRU, curr->hash_value);
            }
            curr = curr->next;
        }	
        if ((cache->max_size - cache->curr_size) >= file_size){
            return true;
        }
        else{
            return false;
        }
    }
}

void LRU_update(struct queue *LRU, int idx){
    struct queue *last;
    struct queue *new = (struct queue*)malloc(sizeof(struct queue));
    new->hash_value = idx;
    new->next = NULL;

    if(LRU->next == NULL) {
        LRU->next = new;
        return;
    }
    last = LRU->next; 
    while(last->next != NULL) {
        last = last->next;
    }
    last->next = new;
}

void LRU_replace(struct queue *LRU, int idx){
    queue_pop(LRU, idx);
    LRU_update(LRU, idx);
}
 
static void do_server_request(struct server *sv, int connfd) 
{
    int ret;
    struct request *rq;
    struct file_data *data;

    data = file_data_init();

    /* fill data->file_name with name of the file being requested */
    rq = request_init(connfd, data);
    if (!rq) {
        file_data_free(data);
        return;
    }

    if(sv->max_cache_size > 0){
        pthread_mutex_lock(&lock);
        struct file *target = cache_lookup(sv, data->file_name); 		
        if (target != NULL){ 
            data = target->data; 
            request_set_data(rq, data); 
            LRU_replace(sv->web_cache->LRU, hashing(sv, data->file_name));
        }
        else{ 
            pthread_mutex_unlock(&lock);
            ret = request_readfile(rq);
            if (ret == 0){
                goto out;
            }
            pthread_mutex_lock(&lock);
            if (cache_insert(sv, data)){
                LRU_replace(sv->web_cache->LRU, hashing(sv, data->file_name));
            }
        }
        pthread_mutex_unlock(&lock);
        request_sendfile(rq);
    }
    else {
        ret = request_readfile(rq);
        if (ret == 0) { /* couldn't read file */
            goto out;
        }
        /* send file to client */
        request_sendfile(rq);
    }
out:
    request_destroy(rq);
    //file_data_free(data);
} 


struct server *
server_init(int nr_threads, int max_requests, int max_cache_size)
{
	struct server *sv;
	int i;

	sv = Malloc(sizeof(struct server));
	sv->nr_threads = nr_threads;
	/* we add 1 because we queue at most max_request - 1 requests */
	sv->max_requests = max_requests + 1;
	sv->max_cache_size = max_cache_size;
	sv->exiting = 0;

	/* Lab 4: create queue of max_request size when max_requests > 0 */
	sv->conn_buf = Malloc(sizeof(*sv->conn_buf) * sv->max_requests);
	for (i = 0; i < sv->max_requests; i++) {
		sv->conn_buf[i] = -1;
	}
	sv->request_head = 0;
	sv->request_tail = 0;

	/* Lab 5: init server cache and limit its size to max_cache_size */

	/* Lab 4: create worker threads when nr_threads > 0 */
	pthread_mutex_init(&sv->mutex, NULL);
	pthread_cond_init(&sv->prod_cond, NULL);
	pthread_cond_init(&sv->cons_cond, NULL);	
	sv->threads = Malloc(sizeof(pthread_t) * nr_threads);
	for (i = 0; i < nr_threads; i++) {
            SYS(pthread_create(&(sv->threads[i]), NULL, do_server_thread, (void *)sv));
	}
        if (max_cache_size > 0) {
            sv->web_cache = (struct cache *)malloc(sizeof(struct cache));
            sv->web_cache->LRU = (struct queue *)malloc(sizeof(struct queue));
            HASH_SIZE = max_cache_size;
            sv->web_cache->hashtable_size = HASH_SIZE;
            sv->web_cache->hashtable = (struct file**)malloc(HASH_SIZE * sizeof(struct file*));
            sv->web_cache->curr_size = 0;
            sv->web_cache->max_size = max_cache_size;
            sv->web_cache->LRU->next = NULL;
            for(int i = 0; i < HASH_SIZE; i++) {
                sv->web_cache->hashtable[i] = NULL;
            }
        }
	return sv;
}

static void *
do_server_thread(void *arg)
{
	struct server *sv = (struct server *)arg;
	int connfd;

	while (1) {
		pthread_mutex_lock(&sv->mutex);
		while (sv->request_head == sv->request_tail) {
			/* buffer is empty */
			if (sv->exiting) {
				pthread_mutex_unlock(&sv->mutex);
				goto out;
			}
			pthread_cond_wait(&sv->cons_cond, &sv->mutex);
		}
		/* get request from tail */
		connfd = sv->conn_buf[sv->request_tail];
		/* consume request */
		sv->conn_buf[sv->request_tail] = -1;
		sv->request_tail = (sv->request_tail + 1) % sv->max_requests;
		
		pthread_cond_signal(&sv->prod_cond);
		pthread_mutex_unlock(&sv->mutex);
		/* now serve request */
		do_server_request(sv, connfd);
	}
out:
	return NULL;
}

void
server_request(struct server *sv, int connfd)
{
	if (sv->nr_threads == 0) { /* no worker threads */
		do_server_request(sv, connfd);
	} else {
		/*  Save the relevant info in a buffer and have one of the
		 *  worker threads do the work. */

		pthread_mutex_lock(&sv->mutex);
		while (((sv->request_head - sv->request_tail + sv->max_requests)
			% sv->max_requests) == (sv->max_requests - 1)) {
			/* buffer is full */
			pthread_cond_wait(&sv->prod_cond, &sv->mutex);
		}
		/* fill conn_buf with this request */
		assert(sv->conn_buf[sv->request_head] == -1);
		sv->conn_buf[sv->request_head] = connfd;
		sv->request_head = (sv->request_head + 1) % sv->max_requests;
		pthread_cond_signal(&sv->cons_cond);
		pthread_mutex_unlock(&sv->mutex);
	}
}

void server_exit(struct server *sv) {        
    pthread_mutex_lock(&sv->mutex);
    sv->exiting = 1;
    pthread_cond_broadcast(&sv->cons_cond);
    pthread_mutex_unlock(&sv->mutex);
    for (int i = 0; i < sv->nr_threads; i++) {
            pthread_join(sv->threads[i], NULL);
    }

    free(sv->conn_buf);
    free(sv->threads);
    
    struct queue* prev = sv->web_cache->LRU;
    struct queue* curr;  

    while (prev != NULL){  
        curr = prev->next;  
        free(prev);  
        prev = curr;  
    }  
    
    for(int i = 0; i < HASH_SIZE; i++){
        if(sv->web_cache->hashtable[i] != NULL){
            file_data_free(sv->web_cache->hashtable[i]->data);
            free(sv->web_cache->hashtable[i]->name);
            sv->web_cache->hashtable[i] = NULL;
        }
    }
    free(sv->web_cache->hashtable);
    free(sv->web_cache);
    free(sv);
    return; 
}

