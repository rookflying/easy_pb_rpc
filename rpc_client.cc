#include <netinet/in.h> // sockaddr_in
#include <sys/socket.h> // socket functions
#include <arpa/inet.h> // htons
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>

#include "rpc_client.h"

using namespace PBRPC;

inline void RpcCallBack( google::protobuf::Closure *c, int wake_fd) {
	if (c) {
		c->Run();
	}else{
		char buf;
		write(wake_fd, &buf, sizeof(buf));
	}
}

inline void ErrorCb(struct bufferevent *bev, short error, void *arg) {
	Session *the_session = (Session *)ptr;
	if (error & BEV_EVENT_EOF) {
		the_session->_bev = NULL;
	} else {
		ERR_LOG("read rpc sock failed with error");
	}
	the_session->DisConnect();
}

inline void ReadCb(struct bufferevent *bev, void *ptr) {
	Session *the_session = (Session *)ptr;
	struct evbuffer *input = bufferevent_get_input(bev);
	the_session->OnCallBack(input);
}

inline void DoRpc(int notifier_read_handle, short event, void *arg) {
	char buf;
	if (read(notifier_read_handle, &buf, 1) <= 0) {
		ERR_LOG("RpcClient DoRpc : read notifier_read_handle failed");
		return;
	}
	RpcClient *this_clt = (RpcClient *)arg;
	CallMessageQueue::Node *msg = this_clt->CallMessageDequeue();

	if (msg->_session_id <= 0 || msg->_session_id > MAX_SESSION_SIZE) {
		ERR_LOG("DoRpc Failed : Wrong session Id");
		return;
	}
	Session *the_session = this_clt->GetSession(msg->_session_id);
	if (msg->_kind == MessageQueue::CONNECT) {
		the_session->Connect(msg);
	} else if (msg->_kind == MessageQueue::CALL) {
		the_session->DoRpcCall(msg);
	}
	delete msg;
}

/*
inline void RecycleSessionTimer(evutil_socket_t fd, short what, void *arg) {
	RpcClient *clt = (RpcClient *)arg;
	clt->DoRecycleSession();
}
*/

inline void * ThreadEntry(void *arg) {
	RpcClient *this_clt = (RpcClient *)arg;
	int notifier_read_handle = this_clt->GetNotifierReadHandle();
	struct event_base *evbase;
	evbase = event_base_new();
	this_clt->SetEventBase(evbase);
	struct event *listener = event_new(evbase, notifier_read_handle, EV_READ|EV_PERSIST,
		DoRpc, arg);
	event_add(listener, NULL);
	/*
	struct timeval interval = {5, 0};
	struct event *timer_event = event_new(evbase, -1, EV_PERSIST, RecycleSessionTimer, this_clt);
	event_add(timer_event, &interval);
	*/
	event_base_dispatch(evbase);
}

void Session::Connect(MessageQueue::Node *conn_msg) {
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr = conn_msg->_connect._ip;
	sin.sin_port = conn_msg->_connect._port;
	bufferevent *bev = bufferevent_socket_new(_evbase, -1, BEV_OPT_CLOSE_ON_FREE);
	bufferevent_setcb(bev, ReadCb, NULL, ErrorCb, this); 
	if (bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin))<0) {
		bufferevent_free(bev);
		conn_msg->_controller->SetFailed("Connect Failed");
	} else 
		the_session->InitSession(bev);
}

void Session::DisConnect() {
	bufferevent_free(_bev);
	_bev = NULL;
}

void Session::DoRpcCall(MessageQueue::Node *call_msg) {
	if (!_bev) {
		call_msg->_controller->SetFailed("Session has disconnect");
		goto CALL_FINALLY;
	}

	unsigned int call_id = 0;
	if (!(call_id = AllocCallId())) {
		call_msg->_controller->SetFailed("Call id alloc failed");
		goto CALL_FINALLY;
	}
	TCallBack *cb = GetCallBack(call_id);
	cb._controller = call_msg->_call._cb._controller;
	cb._response = call_msg->_call._cb._response;
	cb._c = call_msg->_call._cb._c;
	cb._wake_fd = call_msg->_call._cb._wake_fd;

	RPC::RpcRequestData rpc_data;

	rpc_data.set_call_id(call_id);
	rpc_data.set_service_id(call_msg->_call._service_id);
	rpc_data.set_method_id(call_msg->_call._method_id);
	rpc_data.set_content(*call_msg->_call._req_data);
	std::string data_buf;
	rpc_data.SerializeToString(&data_buf);
	if (-1 == bufferevent_write(_bev, data_buf.c_str(), data_buf.size())) {
			call_msg->_controller->SetFailed("bufferevent_write Failed");
	}

CALL_FINALLY:
	if (call_msg->_call._req_data)
		delete call_msg->_call._req_data;
	if (call_msg->_controller->Failed()) {
		RpcCallBack(cb._c, cb._wake_fd);
		if (call_id != 0)
			FreeCallId();
	}
}

void Session::OnCallBack(struct evbuffer *input) {
	size_t buf_len;
	for (;;) {
		switch (_state) {
			case Session::ST_HEAD: {
				buf_len = evbuffer_get_length(input);
				if (buf_len < HEAD_LEN) return ;
				evbuffer_remove(input, _data_length, HEAD_LEN);
				_state = Session::ST_DATA;
				break;
			}
			case Session::ST_DATA: {
				buf_len = evbuffer_get_length(input);
				if (buf_len < _data_length) return ;
				RPC::RpcResponseData rpc_data;
				rpc_data.ParseFromArray(evbuffer_pullup(input, _data_length));
				Session::TCallBack *cb = GetCallBack(rpc_data.call_id());
				if (cb) {
					cb->_response->ParseFromString(rpc_data.content());
					RpcCallBack(cb->_c, cb->_wake_fd);
				}
				FreeCallId(rpc_data.call_id()); //TODO: Destroy Session
				_state = Session::ST_HEAD;
				break;
			}
			default: {
				ERR_LOG("Error Session state");
				break;
			}
		}
	}
}

RpcClient():_sessions(MAX_SESSION_SIZE),_is_start(false),_nrecycle_ss(0) {
}

~RpcClient() {
	event_base_free(_evbase);
}

bool RpcClient::Start(::google::protobuf::RpcController *controller) {
	if (_is_start) return true; // Double check
	_start_mutex.Lock();
	if (!_is_start) {
		if (pipe(_notifier_pipe) != 0) {
			controller->SetFailed("Create Notifier pipe Failed");
			return false;
		}
		pthread_t tid;
		if (0 != pthread_create(&tid, NULL, ThreadEntry, this)) {
			controller->SetFailed("Create Thread Failed");
			return false;
		}
		_is_start = true;
	}
	_start_mutex.Unlock();
	return true;
}


bool RpcClient::CallMsgEnqueue(unsigned int session_id, std::string *req_data, 
	unsigned int service_id, unsigned int method_id,
	google::protobuf::RpcController *controller, google::protobuf::Message *response,
	google::protobuf::Closure *c, int wake_fd) {
	Node *message = new Node;
	message->_kind = MessageQueue::CALL;
	message->_session_id = session_id;
	message->_call._req_data = req_data;
	message->_call._service_id = service_id;
	message->_call._method_id = method_id;
	message->_call._controller = controller;
	message->_call._cb._response = response;
	message->_call._cb._c = c;
	message->_call._cb._wake_fd = wake_fd;
	bool success;
	_msg_queue_mutex.Lock();
	success = _msg_queue.Enqueue(message);
	_msg_queue_mutex.Unlock();
	if (!success) {
		delete message;
		controller->SetFailed("Call Message Enqueue Failed");
		return false;
	}
	char buf;
	if (write(_notifier_pipe[1], &buf, 1) <= 0) {
		controller->SetFailed("Write notifier_write_handle");
		return false;
	}
	return true;
}

bool RpcClient::ConnectMsgEnqueue(unsigned int session_id, 
									google::protobuf::RpcController *controller, 
									struct in_addr ip,
									unsigned short port) {

	Node *message = new Node;
	message->_kind = MessageQueue::CONNECT;
	message->_session_id = session_id;
	message->_controller = controller;
	message->_connect._ip = ip;
	message->_connect._port = port;
	bool success;
	_msg_queue_mutex.Lock();
	success = _msg_queue.Enqueue(message);
	_msg_queue_mutex.Unlock();
	if (!success) {
		delete message;
		ERR_LOG("Connect Msg Enqueue Failed");
		return false;
	}
	char buf;
	if (write(_notifier_pipe[1], &buf, 1) <= 0) {
		ERR_LOG("ConnectMsgEnqueue, Write notifier_write_handle");
		return false;
	}
	return true;
}

MessageQueue::Node *RpcClient::MessageDequeue() {
	MessageQueue::Node *node;
	_msg_queue_mutex.Lock();
	node = _msg_queue.Dequeue();
	_msg_queue_mutex.Unlock();
	return node;
}

