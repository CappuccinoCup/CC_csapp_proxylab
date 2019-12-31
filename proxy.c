#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_STRUCT_NUM 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
/* requested HTTP header format */
static const char *requestLine_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *proxyConn_hdr = "Proxy-Connection: close\r\n";

/* data structure and functions of cache section */
struct cache_struct
{   //double linked list
    char cache_object[MAX_OBJECT_SIZE];
    char cache_uri[MAXLINE];
    struct cache_struct *prev;
    struct cache_struct *next;
};
struct Cache
{
    int cache_num;
    struct cache_struct *head;
    struct cache_struct *tail;
    //reader-writer model(refer to the textbook)
    int readcnt;
    sem_t mutex, w;
} cache;

void cache_init();
void cache_add(char *uri, char *cachebuf);
char *cache_find(char *uri);
void cache_delete();
/* data and functions of cache section */

/* some functions */
void *thread(void *vargp);
void doit(int connfd);
void parse_uri(char *uri, int *port, char *hostname, char *filename);
void build_httpHeader(char *http_header, char *hostname, char *filename, int port, rio_t *client_rio);

int main(int argc, char **argv){
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t pid;

    /* check command-line args */
    if(argc != 2){
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    cache_init();

    listenfd = Open_listenfd(argv[1]);
    while(1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);//connect to the client
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        /* sequential handle */
        //doit(connfd);
        //Close(connfd);

        /* concurrent handle */
        int *connfd_value = (int *)malloc(sizeof(int));
        *connfd_value = connfd;//pass the parameter by pointer
        Pthread_create(&pid, NULL, thread, connfd_value);
    }
    return 0;
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);//get the patameter
    free(vargp);
    Pthread_detach(pthread_self());
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int connfd) {
    int port, serverConnfd;
    char hostname[MAXLINE], filename[MAXLINE];
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char uri_for_cache[MAXLINE];
    char *result;
    char httpHeader[MAXLINE];
    rio_t rio, serverRio;

    /* read request line */
    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);

    if(strcasecmp(method,"GET")){
        printf("Proxy only implement method: GET\n");
        return ;
    }

    /* check the cache */
    strcpy(uri_for_cache, uri); //store the original uri
    if((result = cache_find(uri_for_cache)) != NULL) {
        Rio_writen(connfd, result, strlen(result));//wirte to the client
        return ;
    }

    parse_uri(uri, &port, hostname, filename);

    build_httpHeader(httpHeader, hostname, filename, port, &rio);

    /* connect to the server */
    char portStr[12];
    sprintf(portStr, "%d", port);
    serverConnfd = Open_clientfd(hostname, portStr);

    /* write the http header to the server */
    Rio_readinitb(&serverRio, serverConnfd);
    Rio_writen(serverConnfd, httpHeader, strlen(httpHeader));

    /* receive the message and forward to the client */
    char cachebuf[MAX_OBJECT_SIZE];
    int bufSize = 0;
    size_t n;
    while((n = Rio_readlineb(&serverRio, buf, MAXLINE)) != 0)
    {
        bufSize += n;
        if(bufSize < MAX_OBJECT_SIZE) 
            strcat(cachebuf, buf);
        Rio_writen(connfd, buf, n);//wirte to the client
    }

    Close(serverConnfd);

    /* add to cache */
    if(bufSize < MAX_OBJECT_SIZE){
        cache_add(uri_for_cache, cachebuf);
    }
}

void parse_uri(char *uri, int *port, char *hostname, char *filename) {
    *port = 80;//default port

    char *index1 = strstr(uri, "//");
    if(index1 == NULL){
        //e.g., www.cmu.edu:8080/hub/index.html
        index1 = uri;
    } else {
        //e.g., http://www.cmu.edu:8080/hub/index.html
        index1 += 2;
    }

    char *index2 = strstr(index1, ":");//get the index pointer of ":" in the uri
    if(index2 != NULL){
        //e.g., http://www.cmu.edu:8080/hub/index.html
        *index2 = '\0';
        sscanf(index1, "%s", hostname);
        sscanf(index2 + 1, "%d%s", port, filename);
    } else {
        index2 = strstr(index1, "/");
        if(index2 != NULL){
            //e.g., http://www.cmu.edu/hub/index.html
            *index2 = '\0';
            sscanf(index1, "%s", hostname);
            *index2 = '/';
            sscanf(index2, "%s", filename);
        } else {
            //e.g., http://www.cmu.edu
            sscanf(index1, "%s", hostname);

        }
    }
}

void build_httpHeader(char *http_header, char *hostname, char *filename, int port, rio_t *client_rio) {
    char buf[MAXLINE], request_hdr[MAXLINE], host_hdr[MAXLINE], uselessHdrs[MAXLINE];
    
    sprintf(request_hdr, requestLine_hdr_format, filename);//get request line header
    
    /* read additional request header from the client */
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if(strcmp(buf, "\r\n") == 0) 
            break;
        if(!strncasecmp(buf,"Host", strlen("Host")))//contain "Host"
        {
            strcpy(host_hdr, buf);
            continue;
        }

        if(!strncasecmp(buf,"Connection", strlen("Connection"))
                &&!strncasecmp(buf, "Proxy-Connection", strlen("Proxy-Connection"))
                &&!strncasecmp(buf, "User-Agent", strlen("User-Agent")))
        {
            strcat(uselessHdrs, buf);
        }
    }

    if(strlen(host_hdr) == 0){
        sprintf(host_hdr, host_hdr_format, hostname);
    }

    /* combine the headers together */
    sprintf(http_header,"%s%s%s%s%s%s\r\n",request_hdr,host_hdr,conn_hdr,proxyConn_hdr,user_agent_hdr,uselessHdrs);
}

void cache_init(){
    cache.head = (struct cache_struct *)malloc(sizeof(struct cache_struct));
    cache.tail = cache.head;
    cache.head->prev = cache.head->next = NULL;
    cache.cache_num = 0;
    cache.readcnt = 0;
    Sem_init(&cache.mutex, 0, 1);
    Sem_init(&cache.w, 0, 1);
}
void cache_add(char *uri, char *cachebuf) {
    P(&cache.w);
    if(cache.cache_num == 0){
        //first cache
    	strcpy(cache.head->cache_uri, uri);
    	strcpy(cache.head->cache_object, cachebuf);
        cache.cache_num++;
        V(&cache.w);
        return;
    }
    if(cache.cache_num == MAX_CACHE_STRUCT_NUM)
        cache_delete();
    //add a new cache to the head
    struct cache_struct *newHead = (struct cache_struct *)malloc(sizeof(struct cache_struct));
    strcpy(newHead->cache_uri, uri);
    strcpy(newHead->cache_object, cachebuf);
    newHead->prev = NULL;
    newHead->next = cache.head;
    cache.head->prev = newHead;
    cache.head = newHead;
    cache.cache_num++;
    V(&cache.w);
}
void cache_delete(){
    //delete the cache on the tail
    struct cache_struct *oldTail = cache.tail;
    cache.tail = oldTail->prev;
    cache.tail->next = NULL;
    free(oldTail);
    cache.cache_num--;
}
char *cache_find(char *uri) {
    P(&cache.mutex);
    cache.readcnt++;
    if(cache.readcnt == 1)//first in
        P(&cache.w);
    V(&cache.mutex);
    struct cache_struct *p = cache.head;
    int length = cache.cache_num;
    for(int i = 0;i < length;i++) {
    	if(strcmp(uri, p->cache_uri) == 0){
            P(&cache.mutex);
            cache.readcnt--;
            if(cache.readcnt == 0)//last out
                V(&cache.w);
            V(&cache.mutex);
    		return p->cache_object;
		}
		p = p->next;
	}
    P(&cache.mutex);
    cache.readcnt--;
    if(cache.readcnt == 0)//last out
        V(&cache.w);
    V(&cache.mutex);
    return NULL;
}