/* Copyright 2011 SRI International
 * See ICENSE for other credits and copying information
 */

#include "util.h"

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

#include <errno.h>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "curl_util.h"
#include "http_parser/http_parser.h"

#ifdef _WIN32
# undef near
# undef far
# define ENOTCONN WSAENOTCONN
#endif

/** 
   This program is used by the integration test harness.  It opens one
   listening socket (the "far" socket) and one outbound connection
   (the "near" socket).  Then it requests web pages whose url is given 
   by standard input using curl, on the near socket, the far socket 
   receives the request and request  it (using curl) from the server. 
   Then it store the result in fartext and  write the result in the the
   far socket. Finally the program receives the result in the near socket
   and store in neartext.
   At the end the program compare the near and far text.

   The program operate in two mode.

   Mode 1: The program open the socket and hand them to curl.
   Mode 2: The program set the socket as SOCKS proxy in curl.

   because it's curl who listen on near socket we do not need
   to get involved with that in bufferevent level. But also
   we need none-blocking so let the far also progress, then
   we need fifo approach.
*/

#define TL_TIMEOUT 0
#define LOGGING 1
class WebpageFetcher
{
public:

  WebpageFetcher()
    : base_stopped(false)
  {
  };
  bufferevent *near = nullptr;
  bufferevent *far = nullptr;
  bufferevent *outbound_far = nullptr;

  evutil_socket_t nearfd = 0;
  evutil_socket_t farfd = 0;
  
  curl_socket_t outbound_farfd;
  
  event* fetch_through_st_event = nullptr;
  event* outbound_far_event = nullptr;

  struct evbuffer *neartext = nullptr;
  struct evbuffer *fartext = nullptr;

  CURLM* _curl_multi_handle = nullptr; //we need the multi so we have none
  //blocking curl
  int _curl_running_handle = 0;

  CURL* curl_near = nullptr;
  CURL* curl_far = nullptr; //far doesn't need to be non-blocking
  CURL* curl_outbound_far = nullptr;

  struct evbuffer *neartrans = nullptr;
  struct evbuffer *fartrans = nullptr;

  evbuffer* http_request = nullptr;
  
  char *lbuf = nullptr;
  size_t lbufsize = 0;

  struct evconnlistener *listener = nullptr;
  struct event *pause_timer = nullptr;
  struct event *timeout_timer = nullptr;
  //stop the program in case of communication
  //error
  struct event_base *base = nullptr;
  bool base_stopped = false;

  bool rcvd_eof_near : 1;
  bool rcvd_eof_far  : 1;
  bool sent_eof_near : 1;
  bool sent_eof_far  : 1;
  bool script_eof    : 1;
  bool saw_error     : 1;

  static bool send_curl();
  static bool recv_curl();

  bool init_easy_set_socket(CURL*& cur_curl_handle,  bufferevent* bufferside);

  bool fetch_page();
  bool fetch_direct_socket();

  bool fetch_through_st(string page_uri);

  int compare_far_near();
  
};

class ParsedHeader {

protected:
  string _url;
  string _host;
  bool _host_found;
  
public:
  bool _message_complete;
  http_parser_settings settings_url_extract;

protected:
  http_parser* _parser;

  /** socket_read_cb ask for parsing the incoming info
      we only take action if the message is complete so 
      all the acctions are starting here */
  static int message_complete_cb (http_parser *p)
  {
     ((ParsedHeader*)(p->data))->_message_complete = true;
    return 0;
  }

  static int request_url_cb (http_parser *p, const char *buf, size_t len)
  {
    ((ParsedHeader*)(p->data))->_url.assign(buf, len);
    return 0;
  }

  static int
  header_field_cb (http_parser *p, const char *buf, size_t len)
  {
    if (!strncmp(buf, "Host", len))
      ((ParsedHeader*)(p->data))->_host_found = true;

    return 0;
  }

  static int
  header_value_cb (http_parser *p, const char *buf, size_t len)
  {

    if (((ParsedHeader*)(p->data))->_host_found){
      ((ParsedHeader*)(p->data))->_host.assign(buf, len);
      ((ParsedHeader*)(p->data))->_host_found = false;
    }

    return 0;
  }

public:
  ParsedHeader()
  {
    
    memset(&settings_url_extract, 0, sizeof(http_parser_settings));
    settings_url_extract.on_header_field = header_field_cb;
    settings_url_extract.on_header_value = header_value_cb;
    settings_url_extract.on_url = request_url_cb;
    settings_url_extract.on_message_complete = message_complete_cb;

    _parser = new http_parser;
    _parser->data = this;

    http_parser_init(_parser, HTTP_REQUEST);
  }

  ~ParsedHeader()
  {
    delete _parser;
  }

  bool
  extract_url(evbuffer* get_request, string& extracted_url, string& host) 
  {
    //first assume that message is incomplete unless we found
    //out otherwise
    _message_complete = false;
    char* linear_req = (char*) evbuffer_pullup(get_request, -1);

    size_t nparsed;

    nparsed = http_parser_execute(_parser, &settings_url_extract, linear_req, evbuffer_get_length(get_request));

    if (!_message_complete)
      return false;

    if (nparsed == evbuffer_get_length(get_request)) {
      if (_url.substr(0, sizeof "http://")=="http://") {
          extracted_url = _url;
      }
      else {          host = _host;

        extracted_url = "http://";
        extracted_url += _host;
        extracted_url += _url;
      }
      host = _host;

        return true;
            
    }
    return false;
  }

};

static void
send_squelch(WebpageFetcher *st, struct bufferevent *bev)
{
  evutil_socket_t fd = bufferevent_getfd(bev);
  bufferevent_disable(bev, EV_READ);
  if (fd == -1)
    return;
  if (shutdown(fd, SHUT_RD) &&
      EVUTIL_SOCKET_ERROR() != ENOTCONN) {
    //flush_text(st, bev == st->near);
    fprintf(stderr,
                        "%c W sending squelch: %s\n",
                        bev == st->near ? '{' : '}',
                        evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  }
}

curl_socket_t get_conn_socket(void *bufferside,
                                curlsocktype purpose,
                                struct curl_sockaddr *address)
{
  (void)purpose;
  //We just igonre the address because the connection has been established
  //before hand.
  (void)address;
  curl_socket_t conn_sock = (curl_socket_t)bufferevent_getfd((bufferevent*)bufferside);//In case Zack doesn't like the idea of adding function to conn_t: (curl_socket_t)(bufferevent_getfd(((conn_t*)conn)->buffer));
  return conn_sock;
}

size_t
curl_read_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  if (LOGGING) 
    fprintf(stderr, "received %lu bytes\n", size * nmemb);

  return evbuffer_add((evbuffer*)userdata, ptr, size * nmemb) ?  0 : size * nmemb;
}

/*
  This function needs to be called twice to setup the near and the
  far handles
**/
bool WebpageFetcher::init_easy_set_socket(CURL*& cur_curl_handle,  bufferevent* bufferside)
{
  //setting up near handle to be a part of multi handle
  cur_curl_handle  = curl_easy_init();
  if (!cur_curl_handle) {
    fprintf(stderr, "failed to initiate curl\n");
    return false;
  }
  
  curl_easy_setopt(cur_curl_handle, CURLOPT_HEADER, 1L);
  curl_easy_setopt(cur_curl_handle, CURLOPT_HTTP_CONTENT_DECODING, 0L);
  curl_easy_setopt(cur_curl_handle, CURLOPT_HTTP_TRANSFER_DECODING, 0L);
  curl_easy_setopt(cur_curl_handle, CURLOPT_WRITEFUNCTION, curl_read_cb);
  curl_easy_setopt(cur_curl_handle, CURLOPT_WRITEDATA, neartext);

  curl_easy_setopt(cur_curl_handle, CURLOPT_OPENSOCKETFUNCTION, get_conn_socket);
  curl_easy_setopt(cur_curl_handle, CURLOPT_OPENSOCKETDATA, bufferside);

  //tells curl the socket is already connected
  curl_easy_setopt(cur_curl_handle, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
  curl_easy_setopt(cur_curl_handle, CURLOPT_CLOSESOCKETFUNCTION, curl_close_socket_cb);

  if (LOGGING >= 1)
    curl_easy_setopt(cur_curl_handle, CURLOPT_VERBOSE, 1L);

  return true;

}

static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp)
{
  const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };
  (void)sockp;
  (void) cbp;

  if (LOGGING >= 1)
    fprintf(stderr,
            "socket callback: s=%d e=%p what=%s \n", s, e, whatstr[what]);

  if (what == CURL_POLL_REMOVE) {
    //event_del(((WebpageFetcher*)cbp)->fetch_through_st_event);
    //curl_multi_cleanup(((WebpageFetcher*)cbp)->_curl_multi_handle);
  }

  return 0;
}

/* Called by libevent when we get action on a multi socket */ 
void curl_socket_event_cb(int fd, short kind, void *userp)
{
  CURLMcode rc;
  WebpageFetcher* st = (WebpageFetcher*) userp;

  //fprintf(stderr, "socket is ready for %s\n", kind & EV_READ ? "read" : (kind & EV_WRITE ? "write" : "unknow op"));

  int action =
    (kind & EV_READ ? CURL_CSELECT_IN : 0) |
    (kind & EV_WRITE ? CURL_CSELECT_OUT : 0);

  rc = curl_multi_socket_action(st->_curl_multi_handle, fd, action, &st->_curl_running_handle);

  if (rc != CURLM_OK)
      fprintf(stderr, "error in requesting the uri. CURL Error %s\n", curl_multi_strerror(rc));

  //fprintf(stderr, "%d handles active.\n", st->_curl_running_handle);

   if (st->_curl_running_handle == 0 && !st->base_stopped) { //done with the transfer go to comparision module
     //evbuffer_add_buffer(st->neartext, bufferevent_get_input(st->near));
     //shutdown(fd, SHUT_RD | SHUT_WR);
     //event_del(st->fetch_through_st_event);
     event_base_loopexit(st->base, 0);
    }

}

bool fetch_page(CURL* curl_easy_handle, evbuffer* webpage, string url)
{

  curl_easy_setopt(curl_easy_handle, CURLOPT_WRITEDATA, &webpage);
  curl_easy_setopt(curl_easy_handle, CURLOPT_URL, url.c_str());

  return (curl_easy_perform(curl_easy_handle) == CURLE_OK);
  
}

/*bool fetch_page_direct(evbuffer* webpage)
{
  CURL* curl_easy_handle = curl_easy_init();
  if (!curl_easy_handle){
    fprintf(stderr, "failed to initiate curl");
    return false;
  }
  
  return fetch_page(curl_easy_handle, webpage);

  }*/


bool WebpageFetcher::fetch_through_st(string webpage_uri)
{
  curl_easy_setopt(curl_near, CURLOPT_URL, webpage_uri.c_str());

  CURLMcode res = curl_multi_add_handle(_curl_multi_handle, curl_near);

  if (res != CURLM_OK) {
    fprintf(stderr, "error in adding curl handle. CURL Error %s\n", (const char*)curl_multi_strerror(res));
    return false;
  }

  fetch_through_st_event = event_new(base, nearfd, EV_WRITE | EV_READ | EV_PERSIST, curl_socket_event_cb, this);
  event_add(fetch_through_st_event, NULL);

  curl_easy_setopt(curl_near, CURLOPT_CLOSESOCKETDATA, fetch_through_st_event);


  if (LOGGING >= 1)
    fprintf(stderr, "curl is fetching %s\n", webpage_uri.c_str());

  return true;
}

static void
send_eof(WebpageFetcher *st, struct bufferevent *bev)
{
  evutil_socket_t fd = bufferevent_getfd(bev) ;
  bufferevent_disable(bev, EV_WRITE);
  if (fd == -1)
    return;
  if (shutdown(fd, SHUT_WR) &&
      EVUTIL_SOCKET_ERROR() != ENOTCONN) {
    //flush_text(st, bev == st->near);
    fprintf(stderr,
                        "%c W sending EOF: %s\n",
                        bev == st->near ? '{' : '}',
                        evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
  }
}


/* Event callbacks */

static void
socket_read_cb(struct bufferevent *bev, void *arg)
{
  WebpageFetcher *st = (WebpageFetcher *)arg;
  /* print out the data for the sake of debug */
  /* first we need the size of the buffer */
  if (LOGGING >= 2)
    {
      size_t buffer_size = evbuffer_get_length(bufferevent_get_input(bev));
      char* debug_buf = new char[buffer_size+1];
      evbuffer_copyout(bufferevent_get_input(bev), (void*) debug_buf, sizeof(char)* buffer_size);
      debug_buf[buffer_size] = '\0';
      fprintf(stderr, "Received on %s: %s\n", bev == st->near ? "near" : "far", debug_buf);

    }
  
  if (LOGGING >= 1) {
    fprintf(stderr, "Received %li bytes on %s\n", evbuffer_get_length(bufferevent_get_input(bev)), bev == st->outbound_far ? "outbound_far" : "far");
  }
  if (bev == st->far) {
    evbuffer_add_buffer(st->http_request, bufferevent_get_input(bev));

    //We need to parse the request to see if received everything
    if (!st->fetch_direct_socket() && LOGGING)
      fprintf(stderr, "Couldn't fetch on far, (maybe incomplete).\n");

  }
  else {
    evbuffer_add(st->fartext, evbuffer_pullup(bufferevent_get_input(bev),-1), evbuffer_get_length(bufferevent_get_input(bev)));
    evbuffer_add_buffer(bufferevent_get_output(st->far), bufferevent_get_input(bev));
  }


}

static void
socket_drain_cb(struct bufferevent *bev, void *arg)
{
  WebpageFetcher *st = (WebpageFetcher *)arg;
  (void) st;

  size_t bt =evbuffer_get_length(bufferevent_get_output(bev));
  if (LOGGING >= 1)
    cout << "still has " << bt << " bytes to drain" << endl;

  if (evbuffer_get_length(bufferevent_get_output(bev)) > 0)
    return;

}

static void
socket_event_cb(struct bufferevent *bev, short what, void *arg)
{
  WebpageFetcher *st = (WebpageFetcher *)arg;
  bool reading = (what & BEV_EVENT_READING);
  bool far = bev == st->far;
  //struct evbuffer *log = st->fartrans;

  //flush_tex(st, near);
  what &= ~(BEV_EVENT_READING|BEV_EVENT_WRITING);

  /* EOF, timeout, and error all have the same consequence: we stop
     trying to transmit or receive on that socket, and notify TCP of
     this as well. */
  if (what & (BEV_EVENT_EOF|BEV_EVENT_TIMEOUT|BEV_EVENT_ERROR)) {
    if (what & BEV_EVENT_EOF) {
      what &= ~BEV_EVENT_EOF;
      if (reading)
        {
          if (LOGGING)
              {
                fprintf(stderr,"%c\n", far ? '[' : ']');
              }
        }
      else
        fprintf(stderr, "%c S\n", far ? '{' : '}');
    }
    if (what & BEV_EVENT_ERROR) {
      what &= ~BEV_EVENT_ERROR;
      fprintf(stderr, "%c %c %s\n",
                          far ? '{' : '}', reading ? 'R' : 'W',
                          evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }
    if (what & BEV_EVENT_TIMEOUT) {
      what &= ~BEV_EVENT_TIMEOUT;
      fprintf(stderr, "%c %c --timeout--\n",
                          far ? '{' : '}', reading ? 'R' : 'W');
    }

    send_squelch(st, bev);
    send_eof(st, far ? st->outbound_far : st->far);
      
    // if (near)
    //     st->rcvd_eof_near = true;
    // else
    //     st->rcvd_eof_far = true;
  } else if (what!=BEV_EVENT_CONNECTED){
    send_eof(st, bev);
    send_squelch(st, NULL);
    // if (near)
    //   st->sent_eof_near = true;
    // else
    //   st->sent_eof_far = true;
  }

  /* connect is just logged */
  /*if (what & BEV_EVENT_CONNECTED) {
    what &= ~BEV_EVENT_CONNECTED;
    evbuffer_add_printf(log, "%c\n", near ? '(' : ')');
    }*/

  /* unrecognized events are also just logged */
  /*if (what) {
    evbuffer_add_printf(log, "%c %c unrecognized events: %04x\n",
                        near ? '{' : '}', reading ? 'R' : 'W', what);
                        }*/

}

/*static void
pause_expired_cb(evutil_socket_t, short, void *arg)
{
  tstate *st = (tstate *)arg;
    script_next_action(st);
  return;
}
*/

/* Stop the loop print what ever you have */
static void
timeout_cb(evutil_socket_t, short, void *arg)
{
  fprintf(stderr, "Communitation timed out...\n");
  WebpageFetcher *st = (WebpageFetcher *)arg;

  evutil_socket_t fd = bufferevent_getfd(st->near);
  bufferevent_disable(st->near, EV_WRITE);
  if (fd != -1)
    shutdown(fd, SHUT_WR);

  fd = bufferevent_getfd(st->far);
  bufferevent_disable(st->far, EV_WRITE);
  if (fd != -1)
    shutdown(fd, SHUT_WR);

  event_base_loopexit(st->base, 0);
}

void
queue_eof(WebpageFetcher *st, bool near)
{
  struct bufferevent *buf;

  if (near) {
    st->sent_eof_near = true;
    buf = st->near;
  } else {
    st->sent_eof_far = true;
    buf = st->far;
  }

  if (evbuffer_get_length(bufferevent_get_output(buf)) == 0)
    send_eof(st, buf);
  /* otherwise, socket_drain_cb will do it */
}

void
stop_if_finished(WebpageFetcher *st)
{
  if (st->rcvd_eof_near &&
      st->rcvd_eof_far &&
      st->sent_eof_near &&
      st->sent_eof_far &&
      st->script_eof &&
      evbuffer_get_length(bufferevent_get_input(st->near)) == 0 &&
      evbuffer_get_length(bufferevent_get_output(st->near)) == 0 &&
      evbuffer_get_length(bufferevent_get_input(st->far)) == 0 &&
      evbuffer_get_length(bufferevent_get_output(st->far)) == 0)
    event_base_loopexit(st->base, 0);
}

bool WebpageFetcher::fetch_direct_socket()
{
  ParsedHeader get_parser;

  string page_url, host;
  if (!get_parser.extract_url(http_request, page_url, host)) {
     fprintf(stderr, "Error in parsing http get request (maybe incomplet)\n");
    return false;
  }

  outbound_far = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

  if (bufferevent_socket_connect_hostname(outbound_far,
                                 NULL,
                                 AF_UNSPEC,
                                 host.c_str(),
                                          80))
    {
      fprintf(stderr, "Error in connecting to the host\n");
      return false;
    }

  bufferevent_setcb(outbound_far, socket_read_cb, socket_drain_cb,
                    socket_event_cb, this);

  bufferevent_enable(outbound_far, EV_READ|EV_WRITE);


  evbuffer_add_buffer(bufferevent_get_output(outbound_far), http_request);
  return true;

}

/* I have the plan to fifizing in future */
/* This gets called whenever data is received from the fifo */ 
// static void fifo_cb(int fd, short event, void *arg)
// {
//   char s[1024];
//   long int rv=0;
//   int n=0;
//   GlobalInfo *g = (GlobalInfo *)arg;
//   (void)fd; 
//   (void)event; 
 
//   do {
//     s[0]='\0';
//     rv=fscanf(g->input, "%1023s%n", s, &n);
//     s[n]='\0';
//     if ( n && s[0] ) {
//       new_conn(s,arg); /* if we read a URL, go get it! */ 
//     } else break;
//   } while ( rv != EOF);
// }

static void
init_sockets_internal(WebpageFetcher *st)
{
  /* The behavior of pair bufferevents is sufficiently unlike the behavior
     of socket bufferevents that we don't want them here. Use the kernel's
     socketpair() instead. */
  evutil_socket_t pair[2];
  int rv;

#ifdef AF_LOCAL
  rv = evutil_socketpair(AF_LOCAL, SOCK_STREAM, 0, pair);
#else
  rv = evutil_socketpair(AF_INET, SOCK_STREAM, 0, pair);
#endif
  if (rv == -1) {
    fprintf(stderr, "socketpair: %s\n",
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  st->near = bufferevent_socket_new(st->base, pair[0], BEV_OPT_CLOSE_ON_FREE);
  st->far = bufferevent_socket_new(st->base, pair[1], BEV_OPT_CLOSE_ON_FREE);
  if (!st->near || !st->far) {
    fprintf(stderr, "creating socket buffers: %s\n",
            strerror(errno));
    exit(1);
  }
}

static void
init_sockets_external(WebpageFetcher *st, const char *near, const char *far)
{
  /* We don't bother using libevent's async connection logic for this,
     because we have nothing else to do while waiting for the
     connections to happen, so we might as well just block in
     connect() and accept().  [XXX It's possible that we will need to
     change this in order to work correctly on Windows; libevent has
     substantial coping-with-Winsock logic that *may* be needed here.]
     However, take note of the order of operations: create both
     sockets, bind the listening socket, *then* call connect(), *then*
     accept().  The code under test triggers outbound connections when
     it receives inbound connections, so any other order will either
     fail or deadlock. */
  evutil_socket_t listenfd;

  struct evutil_addrinfo *near_addr =
    resolve_address_port(near, 1, 0, "5000");
  struct evutil_addrinfo *far_addr =
    resolve_address_port(far, 1, 1, "5001");

  if (!near_addr || !far_addr)
    exit(2); /* diagnostic already printed */

  st->nearfd = socket(near_addr->ai_addr->sa_family, SOCK_STREAM, 0);
  if (!st->nearfd) {
    fprintf(stderr, "socket(%s): %s\n", near,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }
  listenfd = socket(far_addr->ai_addr->sa_family, SOCK_STREAM, 0);
  if (!listenfd) {
    fprintf(stderr, "socket(%s): %s\n", far,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  if (evutil_make_listen_socket_reuseable(listenfd)) {
    fprintf(stderr, "setsockopt(%s, SO_REUSEADDR): %s\n", far,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }
  if (bind(listenfd, far_addr->ai_addr, far_addr->ai_addrlen)) {
    fprintf(stderr, "bind(%s): %s\n", far,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }
  if (listen(listenfd, 1)) {
    fprintf(stderr, "listen(%s): %s\n", far,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  if (connect(st->nearfd, near_addr->ai_addr, near_addr->ai_addrlen)) {
    fprintf(stderr, "connect(%s): %s\n", near,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  st->farfd = accept(listenfd, NULL, NULL);
  if (st->farfd == -1) {
    fprintf(stderr, "accept(%s): %s\n", far,
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  /* Done listening now. */
  evutil_closesocket(listenfd);
  evutil_freeaddrinfo(near_addr);
  evutil_freeaddrinfo(far_addr);

  /* Now we're all hooked up, switch to nonblocking mode and
     create bufferevents. */
  if (evutil_make_socket_nonblocking(st->nearfd) ||
      evutil_make_socket_nonblocking(st->farfd)) {
    fprintf(stderr, "setsockopt(SO_NONBLOCK): %s\n",
            evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    exit(1);
  }

  st->near = bufferevent_socket_new(st->base, st->nearfd, BEV_OPT_CLOSE_ON_FREE);
  st->far = bufferevent_socket_new(st->base, st->farfd, BEV_OPT_CLOSE_ON_FREE);
  if (!st->near || !st->far) {
    fprintf(stderr, "creating socket buffers: %s\n",
            strerror(errno));
    exit(1);
  }
}

int
WebpageFetcher::compare_far_near()
{

  if (LOGGING >= 2) {
    fprintf(stderr, "@far:\n%s\n", (const char*)evbuffer_pullup(fartext,-1));
    fprintf(stderr, "@near:\n%s\n", (const char*)evbuffer_pullup(neartext,-1));
  }

  return strncmp((const char*)evbuffer_pullup(neartext,-1), (const char*)evbuffer_pullup(fartext, -1), evbuffer_get_length(fartext));
  
}

/*static void fetch_page(WebpageFetcher *st)
{
  
}*/

/**

   After openning the socket we need to initiate our
   curl handles and give them the sockets
 */
bool init_curl_handles(WebpageFetcher *st)
{
  if (!(st->_curl_multi_handle = curl_multi_init())) {
    fprintf(stderr, "failed to initiate curl multi object.\n");
    return false;
  }

  curl_multi_setopt(st->_curl_multi_handle, CURLMOPT_SOCKETFUNCTION, sock_cb);
  curl_multi_setopt(st->_curl_multi_handle, CURLMOPT_SOCKETDATA, st);


  return (st->init_easy_set_socket(st->curl_near, st->near));// && init_easy_set_socket(st->curl_far, st->far));

}

int
main(int argc, char **argv)
{
  WebpageFetcher st;
  //memset(&st, 0, sizeof(WebpageFetcher)); //why even? there is a constructor

  if (argc != 4) {
    char *name = strrchr(argv[0], '/');
    name = name ? name+1 : argv[0];
    fprintf(stderr, "usage: %s near-addr far-addr url\n", name);
    return 2;
  }

  st.base = event_base_new();
  st.neartext = evbuffer_new();
  //st.neartrans = evbuffer_new();
  st.fartext = evbuffer_new();
  st.http_request = evbuffer_new();
  //st.fartrans = evbuffer_new();

  if (!st.base || !st.neartext || //!st.neartrans ||
      !st.fartext ) {//|| !st.fartrans) {
    fprintf(stderr, "creating event base and buffers: %s\n", strerror(errno));
    return 1;
  }
  //st.pause_timer = evtimer_new(st.base, pause_expired_cb, &st);
  st.timeout_timer = evtimer_new(st.base, timeout_cb, &st); //to end the 
  //program in the case of communication  problem
  if (!st.timeout_timer) {
    fprintf(stderr, "creating pause timer or timeout timer: %s\n", strerror(errno));
    return 1;
  }

  if (TL_TIMEOUT)
    {
      struct timeval tv;
      tv.tv_sec = TL_TIMEOUT;
      tv.tv_usec = 0;
      evtimer_add(st.timeout_timer, &tv);

    }

  if (argc == 1)
    init_sockets_internal(&st);
  else
    init_sockets_external(&st, argv[1], argv[2]);

  bufferevent_setcb(st.near,
                    socket_read_cb, socket_drain_cb, socket_event_cb, &st);
  bufferevent_setcb(st.far,
                    socket_read_cb, socket_drain_cb, socket_event_cb, &st);
  //bufferevent_enable(st.near, EV_READ|EV_WRITE); libcurl will listen on near
  bufferevent_enable(st.far, EV_READ|EV_WRITE);

  init_curl_handles(&st);

  string test_uri = argv[3];
  //string test_uri = "http://localhost/HITB-Ezine-Issue-008.pdf";  //"http://127.0.0.1/";
  //string test_uri = //"http://download.cdn.mozilla.net/pub/mozilla.org/firefox/releases/16.0.1/linux-i686/en-US/firefox-16.0.1.tar.bz2"; //"http://speedtest.wdc01.softlayer.com/downloads/test500.zip";
      //"http://www.1112.net/lastpage.html";//"http://gfaster.com";
  //cin >> test_uri;
  if (!st.fetch_through_st(test_uri)) {
       fprintf(stderr, "Error in fetching web page.\n");
       return -1;
  }

  event_base_dispatch(st.base);

  if (st.compare_far_near())
    fprintf(stderr,"Failed: far and near are not the same");

  bufferevent_free(st.near);
  bufferevent_free(st.far);
  //event_free(st.pause_timer);
  evbuffer_free(st.neartext);
  //evbuffer_free(st.neartrans);
  evbuffer_free(st.fartext);
  evbuffer_free(st.http_request);
  //evbuffer_free(st.fartrans);
  event_base_free(st.base);
  //free(st.lbuf);

  //more clean up
  curl_multi_cleanup(st._curl_multi_handle);

  return st.saw_error;
}
