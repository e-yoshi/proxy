/*
 * proxy.c - Web proxy for COMPSCI 512
 *
 */

#include <stdio.h>
#include "csapp.h"
#include <pthread.h>

#define   FILTER_FILE   "proxy.filter"
#define   LOG_FILE      "proxy.log"
#define   DEBUG_FILE	"proxy.debug"
#define   CONNECT_MTHD  "CONNECT"
#define   GET_MTHD      "GET"
#define   POST_MTHD     "POST"
#define   ENDLINE       "\r\n"
#define   MAXTRIES      10



/*============================================================
 * function declarations
 *============================================================*/

int  find_target_address(char * uri,
                         char * target_address,
                         char * path,
                         int  * port);


void  format_log_entry(char * logstring,
                       int sock,
                       char * uri,
                       int size);
void log_activity(int sock, char * uri, int size);
void log_debug(const char *format, ...);

void *forwarder(void* args);
void *webTalk(void* args);
void secureTalk(int clientfd, rio_t client, char *inHost, char *version, int serverPort);
void ignore();

int debug;
int proxyPort;
int debugfd;
int logfd;
pthread_mutex_t log_mutex;
pthread_mutex_t debug_mutex;

/* main function for the proxy program */

int main(int argc, char *argv[])
{
    int listenfd, connfd, clientlen, optval, serverPort;
    struct sockaddr_in clientaddr;
    struct hostent *hp;
    char *haddrp;
    sigset_t sig_pipe;
    pthread_t tid;
    int *thread_args;
    
    if (argc < 2) {
        printf("Usage: ./%s port [debug] [webServerPort]\n", argv[0]);
        exit(1);
    }
    if(argc == 4)
        serverPort = atoi(argv[3]);
    else
        serverPort = 80;
    
    Signal(SIGPIPE, ignore);
    
    if(sigemptyset(&sig_pipe) || sigaddset(&sig_pipe, SIGPIPE))
        unix_error("creating sig_pipe set failed");
    if(sigprocmask(SIG_BLOCK, &sig_pipe, NULL) == -1)
        unix_error("sigprocmask failed");
    
    proxyPort = atoi(argv[1]);
    
    if(argc > 2)
        debug = atoi(argv[2]);
    else
        debug = 0;
    
    /* start listening on proxy port */
    
    listenfd = Open_listenfd(proxyPort);
    
    optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    
    if(debug) debugfd = Open(DEBUG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    
    logfd = Open(LOG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    
    /* if writing to log files, force each thread to grab a lock before writing
     to the files */
    
    pthread_mutex_init(&log_mutex, NULL);
    pthread_mutex_init(&debug_mutex, NULL);
    
    
    while(1) {
        
        clientlen = sizeof(clientaddr);
        thread_args = Malloc(sizeof(2*sizeof(int)));
        /* Accept a new connection from a client here */
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        /* Connection was accepted! */
        hp = Gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr,
                           sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        
        haddrp = inet_ntoa(clientaddr.sin_addr);
        thread_args[0] = connfd;
        thread_args[1] = serverPort;
        Pthread_create(&tid, NULL, webTalk, (void *) thread_args);
        Pthread_detach(tid);
    }
    
    if(debug) Close(debugfd);
    Close(logfd);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&debug_mutex);
    return 0;
}

/* a possibly handy function that we provide, fully written */

void parseAddress(char* url, char* host, char** file, int* serverPort)
{
    char *point1;
    char *point2;
    char *saveptr;
    
    if(strstr(url, "http://"))
        url = &(url[7]);
    *file = strchr(url, '/');
    
    strcpy(host, url);
    
    /* first time strtok_r is called, returns pointer to host */
    /* strtok_r (and strtok) destroy the string that is tokenized */
    
    /* get rid of everything after the first / */
    
    strtok_r(host, "/", &saveptr);
    
    /* now look to see if we have a colon */
    
    point1 = strchr(host, ':');
    if(!point1) {
        *serverPort = 80;
        return;
    }
    
    /* we do have a colon, so get the host part out */
    strtok_r(host, ":", &saveptr);
    
    /* now get the part after the : */
    *serverPort = atoi(strtok_r(NULL, "/",&saveptr));
}



/* this is the function that I spawn as a thread when a new
 connection is accepted */

/* you have to write a lot of it */

void *webTalk(void* args)
{
    int numBytesRead, numBytesWritten, serverfd = -1, clientfd = -1, serverPort, targetPort=0;
    int tries = 0, content_length=0, isPost=0, close_conn=0;
    int byteCount = 0;
    int *thread_args;
    char buf1[MAXLINE], buf2[MAXLINE];
    char method[MAXLINE], host[MAXLINE], path[MAXLINE], version[MAXLINE];;
    char url[MAXLINE], port[MAXLINE];
    char *token;
    rio_t server, client;
    pthread_t tid;
    
    clientfd = ((int*)args)[0];
    serverPort = ((int*)args)[1];
    free(args);
    
    Rio_readinitb(&client, clientfd);
    
    numBytesRead = (int)Rio_readlineb(&client, buf1, MAXBUF);
    
    // Determine protocol (CONNECT or GET)
    log_debug("Incoming Request: %s\n", buf1);
    if(numBytesRead<=0)
        return NULL;
    token = strtok(buf1, " ");
    if(token==NULL){
        unix_error("Could not parse method token!");
        return NULL;
    }
    strcpy(method, token);
    token = strtok(NULL, " ");
    if(token==NULL){
        unix_error("Could not parse URL token!");
        return NULL;
    }
    strcpy(url, token);
    token = strtok(NULL, " ");
    if(token==NULL){
        unix_error("Could not parse version token!");
        return NULL;
    }
    strcpy(version, token);
    
    if(strcmp(method, "POST")==0)
        isPost=1;
    
    if(strcmp(method, GET_MTHD)==0||isPost){
        if(find_target_address(url, host, path, &targetPort)<0){
            unix_error("Problem Parsing GET request");
            return NULL;
        }
        
        log_debug("Host Name: %s\n", host);
        log_debug("Path Name: %s\n", path);
        log_debug("Port Number: %d\n", targetPort);
        
        //Link overrrides targetPort, technically should only work with 80 and 443.
        if(targetPort==443){
            //If 443 redirect to secure Talk?
        }
        
        // GET: open connection to webserver (try several times, if necessary)
        while((serverfd = Open_clientfd(host, serverPort))<0){
            tries++;
            if(tries==MAXTRIES){
                unix_error("Max Number of attempts reached(10)");
                return NULL;
            }
        }
        
        /* GET: Transfer first header to webserver */
        sprintf(buf2, "%s %s %s", method, url, version);
        
        //Initiate rio read
        Rio_readinitb(&server, serverfd);
        //Write header
        Rio_writen(serverfd, buf2, strlen(buf2));
        log_debug(">>>%s",buf2);
        
        // GET: now receive the response
        thread_args = Malloc(sizeof(2*sizeof(int)));
        thread_args[0] = clientfd;
        thread_args[1] = serverfd;
        Pthread_create(&tid, NULL, forwarder, (void *) thread_args);
        
        // GET: Transfer remainder of the request
        while(1){
            numBytesRead = (int) Rio_readlineb(&client, buf2, MAXBUF);
            if(numBytesRead<0){
                unix_error("Error: with reading client");
                return NULL;
            } else if(strstr(buf2, "Connection")||(strstr(buf2, "Proxy-Connection"))){
                close_conn = 1;
                continue;
            } else if (isPost && strstr(buf2, "Content-Length")){
                content_length = atoi(buf2+16);
                
            } else if (strcmp(buf2, ENDLINE)==0){
                if(close_conn){
                    strcpy(buf2, "Connection: Close\r\n");
                    numBytesWritten = (int)Rio_writen(serverfd, buf2, strlen(buf2));
                    byteCount += numBytesWritten;
                }
                log_debug(">>>%s",buf2);
                numBytesWritten = (int)Rio_writen(serverfd, ENDLINE, numBytesRead);
                if(isPost){
                    char bufPost[content_length];
                    numBytesRead = (int) Rio_readlineb(&client, bufPost, content_length);
                    log_debug(">>>%s\n", bufPost);
                    numBytesWritten = (int)Rio_writen(serverfd, bufPost, content_length);
                    byteCount+=numBytesWritten;
                    break;
                }
                break;
            }
            numBytesWritten = (int)Rio_writen(serverfd, buf2, numBytesRead);
            byteCount+=numBytesWritten;
            log_debug(">>>%s",buf2);
        }
        log_activity(serverfd, url, byteCount);
        Pthread_join(tid, NULL);
        
        shutdown(serverfd,1);
        shutdown(clientfd,1);
        close(serverfd);
        close(clientfd);
        return NULL;
        
    } else if(strcmp(method, CONNECT_MTHD)==0){
        log_debug("CONNECT method received\n");
        // CONNECT: call a different function, securetalk, for HTTPS
        token = strtok(url, ":");
        if(token==NULL){
            unix_error("SSL: Could not parse url token!");
            return NULL;
        }
        
        strcpy(host, token);
        token = strtok(NULL, ":");
        if(token==NULL){
            unix_error("SSL: Could not parse number token!");
            return NULL;
        }
        strcpy(port, token);
        serverPort=atoi(port);
        if(serverPort!=443){
            unix_error("HTTP GET asking for port 443: Ignoring");
            return NULL;
        }
        
        secureTalk(clientfd, client, host, version, serverPort);
        return NULL;
    } else{
        unix_error("UNSUPPORTED METHOD");
        return NULL;
    }
    return NULL;
}


/* this function handles the two-way encrypted data transferred in
 an HTTPS connection */

void secureTalk(int clientfd, rio_t client, char *inHost, char *version, int serverPort)
{
    int serverfd, numBytesRead, numBytesWritten;
    int tries=0;
    rio_t server;
    char buf1[MAXLINE];
    pthread_t tid;
    int *thread_args;
    
    if (serverPort == proxyPort)
        serverPort = 443;
    
    /* connecton to webserver */
    while((serverfd = Open_clientfd(inHost, serverPort))<0){
        tries++;
        if(tries==MAXTRIES){
            unix_error("Max Number of attempts reached!");
            return;
        }
    }
    
    Rio_readinitb(&server, serverfd);
    sprintf(buf1, "%s 200 OK\r\n\r\n", version);
    Rio_writen(clientfd, buf1, strlen(buf1));
    
    /* clientfd is browser */
    /* serverfd is server */
    thread_args = Malloc(sizeof(2*sizeof(int)));
    thread_args[0] = clientfd;
    thread_args[1] = serverfd;
    Pthread_create(&tid, NULL, forwarder, (void *) thread_args);
    
    while(1) {
        numBytesRead = (int)Rio_readp(clientfd, buf1, MAXBUF);
        
        numBytesWritten = (int)Rio_writep(serverfd, buf1, numBytesRead);
        
        if(numBytesRead<=0){
            break;
        }
        
        log_debug("<<<%s",buf1);
        
    }
    Pthread_join(tid, NULL);
    shutdown(serverfd,1);
    shutdown(clientfd,1);
    close(serverfd);
    close(clientfd);
    
}

/* this function is for passing bytes from origin server to client */

void *forwarder(void* args)
{
    int serverfd, clientfd;
    int byteCount=0,numBytesRead, numBytesWritten= 0;
    char buf1[MAXLINE];
    clientfd = ((int*)args)[0];
    serverfd = ((int*)args)[1];
    free(args);
    
    while(1) {
        numBytesRead = (int)Rio_readp(serverfd, buf1, MAXBUF);
        
        numBytesWritten = (int)Rio_writep(clientfd, buf1, numBytesRead);
        if(numBytesRead<=0){
            break;
        }
        byteCount+=numBytesWritten;
        log_debug("<<<%s",buf1);
    }
    return NULL;
}

void ignore(){
    ;
}


/*============================================================
 * url parser:
 *    find_target_address()
 *        Given a url, copy the target web server address to
 *        target_address and the following path to path.
 *        target_address and path have to be allocated before they
 *        are passed in and should be long enough (use MAXLINE to be
 *        safe)
 *
 *        Return the port number. 0 is returned if there is
 *        any error in parsing the url.
 *
 *============================================================*/

/*find_target_address - find the host name from the uri */
int  find_target_address(char * uri, char * target_address, char * path,
                         int  * port)

{
    //printf("uri: %s\n",uri);
    
    
    if (strncasecmp(uri, "http://", 7) == 0) {
        char * hostbegin, * hostend, *pathbegin;
        int    len;
        
        /* find the target address */
        hostbegin = uri+7;
        hostend = strpbrk(hostbegin, " :/\r\n");
        if (hostend == NULL){
            hostend = hostbegin + strlen(hostbegin);
        }
        
        len = (int)(hostend - hostbegin);
        
        strncpy(target_address, hostbegin, len);
        target_address[len] = '\0';
        
        /* find the port number */
        if (*hostend == ':')   *port = atoi(hostend+1);
        
        /* find the path */
        
        pathbegin = strchr(hostbegin, '/');
        
        if (pathbegin == NULL) {
            path[0] = '\0';
            
        }
        else {
            pathbegin++;
            strcpy(path, pathbegin);
        }
        return 0;
    }
    target_address[0] = '\0';
    return -1;
}



/*============================================================
 * log utility
 *    format_log_entry
 *       Copy the formatted log entry to logstring
 *============================================================*/

void format_log_entry(char * logstring, int sock, char * uri, int size)
{
    time_t  now;
    char    buffer[MAXLINE];
    struct  sockaddr_in addr;
    unsigned  long  host;
    unsigned  char a, b, c, d;
    int    len = sizeof(addr);
    
    now = time(NULL);
    strftime(buffer, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));
    
    if (getpeername(sock, (struct sockaddr *) & addr, &len)) {
        /* something went wrong writing log entry */
        printf("getpeername failed\n");
        return;
    }
    
    host = ntohl(addr.sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;
    
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d\n", buffer, a,b,c,d, uri, size);
}


void log_debug(const char *format, ...){
    if(debug){
        Pthread_mutex_lock(&debug_mutex);
        char    buffer[MAXLINE];
        time_t  now;
        va_list arg;
        
        now = time(NULL);
        strftime(buffer, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));
        //fprintf(stdout, "%s> ", buffer);
        //fprintf(stdout, format, arg);
        
        Pthread_mutex_unlock(&debug_mutex);
    }
}

void log_activity(int sock, char * uri, int size){
    Pthread_mutex_lock(&log_mutex);
    char logstring[MAXLINE];
    format_log_entry(logstring, sock, uri, size);
    fprintf(stdout, "%s", logstring);
    Pthread_mutex_unlock(&log_mutex);
}


