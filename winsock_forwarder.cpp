#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <sstream>
#include <regex>
#include <winsock2.h>
#include <thread>
#include <queue>
#include <mutex>
#include <cassert>

#pragma comment(lib, "ws2_32.lib")

using namespace std::chrono_literals;

#define HEAP_BUFSIZ (1024 * 1024)

void err(const char* msg) {
	std::cerr << "[ERROR] " << msg << std::endl;
}

void log(const char* msg) {
	std::clog << "[INFO]  " << msg << std::endl;
}

struct forward_info {
	enum protocol_choice {
		TCP, UDP
	} _prot;
	int _listen_addr;
	short _listen_port;
	int _forward_addr;
	short _forward_port;

	static std::shared_ptr<forward_info> make_from_args(int argc, char* argv[]);
};

enum signal_flag {
	OK,
	SHUTDOWN,
	ERR,
};

void tcp_in_handler(SOCKET accepted_sock, std::shared_ptr<std::queue<char>> queue_p,
	std::shared_ptr<std::mutex> mtx_p,
	std::shared_ptr<std::atomic<int>> sigflag_p,
	sockaddr_in accepted_sa)
{
	char buf[BUFSIZ];
	std::shared_ptr<char[]> buff_p(new char[HEAP_BUFSIZ]);
	while (1) {
		if (*sigflag_p == signal_flag::ERR || *sigflag_p == signal_flag::SHUTDOWN) {
			shutdown(accepted_sock, 2);				  // close socket
			return;									  // thread terminate
		}

		int recv_num = recv(accepted_sock, buff_p.get(), HEAP_BUFSIZ, 0);
		if (recv_num == -1) {
			// error
			*sigflag_p = signal_flag::ERR;
			sprintf(buf, "数据传入错误，传入连接被异常地关闭 (%s:%d)", inet_ntoa(accepted_sa.sin_addr), ntohs(accepted_sa.sin_port));
			err(buf);
		} else if (recv_num == 0) {
			// connection shutdown 
			*sigflag_p = signal_flag::SHUTDOWN;
			sprintf(buf, "连接被一个传入端断开 (%s:%d)", inet_ntoa(accepted_sa.sin_addr), ntohs(accepted_sa.sin_port));
			log(buf);
		} else {
			mtx_p->lock();
			for (int i = 0; i < recv_num; i++) {
				queue_p->push(buff_p[i]);
			}
			mtx_p->unlock();
		}
	}
}

void tcp_out_handler(SOCKET forward_sock, 
	std::shared_ptr<std::queue<char>> queue_p, 
	std::shared_ptr<std::mutex> mtx_p,
	std::shared_ptr<std::atomic<int>> sigflag_p,
	sockaddr_in forward_sa)
{
	char buf[BUFSIZ];
	std::shared_ptr<char[]> buff_p(new char[HEAP_BUFSIZ]);
	while (1) {
		if (*sigflag_p == signal_flag::ERR || *sigflag_p == signal_flag::SHUTDOWN) {
			shutdown(forward_sock, 2);				  // close socket
			return;									  // thread terminate
		}

		mtx_p->lock();
		int tosend_num;
		for (tosend_num = 0; tosend_num < HEAP_BUFSIZ; tosend_num++) {
			if (queue_p->empty()) {
				break;
			}
			buff_p[tosend_num] = queue_p->front();
			queue_p->pop();
		}
		mtx_p->unlock();

		if (tosend_num) {
			int send_num = send(forward_sock, buff_p.get(), tosend_num, 0);
			if (send_num == -1) {
				*sigflag_p = signal_flag::ERR;
				sprintf(buf, "数据传出错误，传出连接被异常地关闭 (%s:%d)", inet_ntoa(forward_sa.sin_addr), ntohs(forward_sa.sin_port));
				err(buf);
			}
		} else {
			std::this_thread::sleep_for(1ms);
		}
	}
}

int tcp_loop(std::shared_ptr<forward_info> fi) {
	char buf[BUFSIZ];
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == INVALID_SOCKET) {
		err("建立监听套接字错误");
		return 1;
	}
	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr.S_un.S_addr = fi->_listen_addr;
	sa.sin_port = fi->_listen_port;
	if (bind(listen_sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sockaddr_in))) {
		err("绑定到端口错误");
		return 1;
	}

	if (listen(listen_sock, 0)) {
		err("监听套接字错误");
		return 1;
	}

	sockaddr_in accepted_sa;
	while (1) {
		int accepted_sa_len = sizeof(sockaddr_in);
		SOCKET accepted_sock =
			accept(listen_sock, reinterpret_cast<sockaddr*>(&accepted_sa), &accepted_sa_len);
		if (accepted_sock == INVALID_SOCKET) {
			err("接受连接错误");
		} else {

			sprintf(buf, "接受传入连接 (%s:%d)", inet_ntoa(accepted_sa.sin_addr), ntohs(accepted_sa.sin_port));
			log(buf);

			SOCKET forward_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (forward_sock == INVALID_SOCKET) {
				err("建立转发套接字错误");
				return 1;
			}

			sockaddr_in forward_sa;
			forward_sa.sin_family = AF_INET;
			forward_sa.sin_addr.S_un.S_addr = fi->_forward_addr;
			forward_sa.sin_port = fi->_forward_port;
			if (connect(forward_sock, reinterpret_cast<sockaddr*>(&forward_sa), sizeof(sockaddr_in))) {
				err("连接到转发目标错误");
				shutdown(accepted_sock, 2);
				continue;
			}

			sprintf(buf, "建立传出连接 (%s:%d)", inet_ntoa(forward_sa.sin_addr), ntohs(forward_sa.sin_port));
			log(buf);

			std::shared_ptr<std::queue<char>> queue_p(new std::queue<char>);
			std::shared_ptr<std::mutex> mtx_p(new std::mutex);
			std::shared_ptr<std::atomic<int>> sigflag_p(new std::atomic<int>(0));
			std::thread thr_in(tcp_in_handler, accepted_sock, queue_p, mtx_p, sigflag_p, accepted_sa);
			std::thread thr_out(tcp_out_handler, forward_sock, queue_p, mtx_p, sigflag_p, forward_sa);
			thr_in.detach();
			thr_out.detach();
		}
	}
	return 0;
}

void udp_in_handler(SOCKET recv_sock, 
	std::shared_ptr<std::queue<char>> queue_p,
	std::shared_ptr<std::mutex> mtx_p,
	sockaddr_in recv_sa)
{
	char buf[BUFSIZ];
	std::shared_ptr<char[]> buff_p(new char[HEAP_BUFSIZ]);
	while (1) {
		int recv_sa_len = sizeof(sockaddr_in);
		int recv_num = recvfrom(recv_sock, buff_p.get(), HEAP_BUFSIZ, 0, 
			reinterpret_cast<sockaddr*>(&recv_sa), &recv_sa_len);
		if (recv_num == -1) {
			sprintf(buf, "数据传入错误 (%s:%d)", inet_ntoa(recv_sa.sin_addr), ntohs(recv_sa.sin_port));
			err(buf);
		} else if (recv_num == 0) {
			sprintf(buf, "数据报套接字断开 (%s:%d)", inet_ntoa(recv_sa.sin_addr), ntohs(recv_sa.sin_port));
			log(buf);
		} else {
			mtx_p->lock();
			for (int i = 0; i < recv_num; i++) {
				queue_p->push(buff_p[i]);
			}
			mtx_p->unlock();
		}
	}
}

void udp_out_handler(SOCKET forward_sock,
	std::shared_ptr<std::queue<char>> queue_p,
	std::shared_ptr<std::mutex> mtx_p,
	sockaddr_in forward_sa)
{
	char buf[BUFSIZ];
	std::shared_ptr<char[]> buff_p(new char[HEAP_BUFSIZ]);
	while (1) {
		mtx_p->lock();
		int tosend_num;
		for (tosend_num = 0; tosend_num < HEAP_BUFSIZ; tosend_num++) {
			if (queue_p->empty()) {
				break;
			}
			buff_p[tosend_num] = queue_p->front();
			queue_p->pop();
		}
		mtx_p->unlock();

		if (tosend_num) {
			int send_num = sendto(forward_sock, buff_p.get(), tosend_num, 0, 
				reinterpret_cast<sockaddr*>(&forward_sa), sizeof(sockaddr_in));
			if (send_num == -1) {
				sprintf(buf, "数据传出错误 (%s:%d)", inet_ntoa(forward_sa.sin_addr), ntohs(forward_sa.sin_port));
				err(buf);
			}
		} else {
			std::this_thread::sleep_for(1ms);
		}
	}
}

int udp_loop(std::shared_ptr<forward_info> fi) {
	SOCKET recv_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (recv_sock == INVALID_SOCKET) {
		err("建立监听套接字错误");
		return 1;
	}
	sockaddr_in recv_sa;
	recv_sa.sin_family = AF_INET;
	recv_sa.sin_addr.S_un.S_addr = fi->_listen_addr;
	recv_sa.sin_port = fi->_listen_port;
	if (bind(recv_sock, reinterpret_cast<sockaddr*>(&recv_sa), sizeof(sockaddr_in))) {
		err("绑定到端口错误");
		return 1;
	}

	SOCKET forward_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in forward_sa;
	forward_sa.sin_family = AF_INET;
	forward_sa.sin_addr.S_un.S_addr = fi->_forward_addr;
	forward_sa.sin_port = fi->_forward_port;

	std::shared_ptr<std::queue<char>> queue_p(new std::queue<char>);
	std::shared_ptr<std::mutex> mtx_p(new std::mutex);
	std::thread thr_in(udp_in_handler, recv_sock, queue_p, mtx_p, recv_sa);
	std::thread thr_out(udp_out_handler, forward_sock, queue_p, mtx_p, forward_sa);
	thr_in.join();
	thr_out.join();
	

	return 0;
}

int main(int argc, char *argv[]) {
	char buf[BUFSIZ];
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		return 1;
	}

	std::shared_ptr<forward_info> fi = forward_info::make_from_args(argc, argv);
	if (!fi) {
		return 1;
	}

	sprintf(buf, "%s 转发器 %s -> %s", (fi->_prot == forward_info::protocol_choice::TCP ? "TCP" : "UDP"), argv[2], argv[3]);
	log(buf);
	if (fi->_prot == forward_info::protocol_choice::TCP) {
		tcp_loop(fi);
	} else if (fi->_prot == forward_info::protocol_choice::UDP) {
		udp_loop(fi);
	} else {
		assert(0 && "bad protocol choice");
	}

	WSACleanup();
	return 0;
} 

bool split_ip_port(char * args, std::stringstream & ss_ip, std::stringstream & ss_port) {
	std::regex re("[0-9]{1,3}(\\.[0-9]{1,3}){3}\\:[0-9]+");
	if (!std::regex_match(args, re)) {
		return false;
	}
	bool scanflag = false;
	for (size_t i = 0; i < strlen(args); i++) {
		if (!scanflag) {
			if (args[i] != ':') 
				ss_ip << args[i];
			else 
				scanflag = true;
		} else {
			ss_port << args[i];
		}
	}
	return true;
}

std::shared_ptr<forward_info> forward_info::make_from_args(int argc, char* argv[]) {
	char buf[BUFSIZ];
	if (argc != 4) {
		sprintf(buf, "命令行参数个数错误\n用法: %s [tcp|udp] local_ip:port forward_ip:port", argv[0]);
		err(buf);
		return nullptr;
	}

	std::shared_ptr<forward_info> pfi(new forward_info);
	if (strcmp(argv[1], "tcp") == 0 || strcmp(argv[1], "TCP") == 0) {
		pfi->_prot = forward_info::protocol_choice::TCP;
	} else if (strcmp(argv[1], "udp") == 0 || strcmp(argv[1], "UDP") == 0) {
		pfi->_prot = forward_info::protocol_choice::UDP;
	} else {
		err("不支持的协议类型");
		return nullptr;
	}

	std::stringstream ss_listen_ip, ss_listen_port;
	std::stringstream ss_forward_ip, ss_forward_port;

	if (!split_ip_port(argv[2], ss_listen_ip, ss_listen_port)) {
		err("监听 ip:port 参数格式错误");
		return nullptr;
	}
	pfi->_listen_port = ntohs(atoi(ss_listen_port.str().c_str()));
	pfi->_listen_addr = inet_addr(ss_listen_ip.str().c_str());

	if (!split_ip_port(argv[3], ss_forward_ip, ss_forward_port)) {
		err("转发 ip:port 参数格式错误");
		return nullptr;
	}
	pfi->_forward_port = ntohs(atoi(ss_forward_port.str().c_str()));
	pfi->_forward_addr = inet_addr(ss_forward_ip.str().c_str());

	return pfi;
}

