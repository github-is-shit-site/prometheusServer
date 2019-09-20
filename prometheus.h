#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ev.h>
#include <vector>
#include <string_view>
#include <functional>
#include <unordered_map>

//#include "fu2/function2.hpp"

using namespace std;

namespace Prometheus
{
	class StatServer
	{
	public:
		enum class SocketEvent
		{
			unsubscribe = 0
			,read = 1
			,write = 2
			,accept = 4
		};

		using OnSubscribeHandler = function<void(int fd, int events)>;

		class OutBuff : public vector<char>
		{
		public:
			size_t curPos_ = 0;
		};

		StatServer()
		{
			for (auto& buf : outBuf_)
                                buf.resize(0x10000);
			HttpWriteHeader(outBuf_[curBuff_ & curBuffMask_]);
		}

		void Start(int port)
		{
			StartListerning(port);
		}

                void SetSubscribeHandler(const OnSubscribeHandler& onSubscribeHandler)
		{
			onSubscribeHandler_ = onSubscribeHandler;
		}

		void OnCanAccept()
		{
			int newfd;
			struct sockaddr_storage their_addr;

			socklen_t sin_size = sizeof their_addr;
			newfd = accept(sockfd_, (struct sockaddr *)&their_addr, &sin_size);

			if (onSubscribeHandler_)
				onSubscribeHandler_(newfd, (int)SocketEvent::read|(int)SocketEvent::write);

		}

		void OnCanRead(int fd)
		{
			int status;
                        char readBuffer[promReadBuffSize];
                        do
                        {
                                status = recv(fd, readBuffer, sizeof readBuffer, MSG_DONTWAIT);
			} while (status > 0);

			if (currentClientFd_ != 0)
			{
				OutBuff buff;
				buff.reserve(0x100);
				HttpWriteHeader(buff);
                                write(fd, buff.data(), buff.curPos_);

				if (onSubscribeHandler_)
					onSubscribeHandler_(fd, (int)SocketEvent::unsubscribe);
				shutdown(fd, 1);
				close(fd);
			}

			currentClientFd_ = fd;

			curBuff_++;
			size_t curBuff = curBuff_ & curBuffMask_;
                        outBuf_[curBuff].curPos_ = 0;
			HttpWriteHeader(outBuf_[curBuff]);

			writePos_ = 0;
			OnCanWrite(fd);

		}

		void OnCanWrite(int fd)
		{
			auto& buff = outBuf_[(curBuff_ - 1)& curBuffMask_];
                        ssize_t writen = write(fd, buff.data() + writePos_, buff.curPos_ - writePos_);
			if (writen <= 0 || writen == buff.curPos_ - writePos_)
			{
				if (onSubscribeHandler_)
					onSubscribeHandler_(fd, (int)SocketEvent::unsubscribe);
				shutdown(fd, 1);
				close(fd);
				currentClientFd_ = 0;
			}
			else
				writePos_ += writen;
		}

		OutBuff& CurBuff()
		{
			return outBuf_[curBuff_ & curBuffMask_];
		}

	protected:
                static constexpr int promConnBacklog = 1;
                static constexpr size_t promReadBuffSize = 0x1000;

                static constexpr size_t curBuffMask_ = 1;
		OutBuff outBuf_[curBuffMask_ + 1];
		size_t  curBuff_ = 0;
		size_t writePos_ = 0;

		int sockfd_;
		int currentClientFd_ = 0;
		OnSubscribeHandler onSubscribeHandler_;

		//void OnSocketWatcherRequest(int mode) noexcept{}
		//void OnConnected() noexcept{}

		int StartListerning(int port)
		{
			int yes=1;
			int gai_ret;
			struct addrinfo hints, *servinfo, *p;

			memset(&hints, 0, sizeof hints);
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_flags = AI_PASSIVE;

			array<char,10> port_str;
			sprintf(port_str.data(), "%d", port);

			gai_ret = getaddrinfo(NULL, port_str.data(), &hints, &servinfo);
			if (gai_ret != 0)
			{
				cerr << "getaddrinfo: " << gai_strerror(gai_ret) << endl;
				return -1;
			}

			for (p = servinfo; p != NULL; p = p->ai_next)
			{

				if ((sockfd_ = socket(p->ai_family, p->ai_socktype,
									 p->ai_protocol)) == -1)
				{
					continue;
				}

				if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &yes,
							   sizeof(int)) == -1)
				{
					perror("setsockopt");
					close(sockfd_);
					freeaddrinfo(servinfo);
					return -2;
				}

				if (bind(sockfd_, p->ai_addr, p->ai_addrlen) == -1)
				{
					close(sockfd_);
					continue;
				}

				break;
			}

			freeaddrinfo(servinfo);

			if (p == NULL)
			{
				cerr << "webserver: failed to find local address\n";
				return -3;
			}

			if (listen(sockfd_, promConnBacklog) == -1) {
				close(sockfd_);
				return -4;
			}

			if (onSubscribeHandler_)
				onSubscribeHandler_(sockfd_, (int)SocketEvent::accept);
		}

                void HttpWriteHeader(OutBuff& buf)
		{
			constexpr char header[] ="HTTP/1.1 200 OK\n\n";
			strcpy( buf.data(), header);
			buf.curPos_ += sizeof(header) - 1;
		}


	};

	class StatServerLibEv : public StatServer
	{
	public:
		void Start(struct ev_loop* loop, int port)
		{
			loop_ = loop;

			//ev_async_init(&asyncNotifier_, OnAsyncNotifier_);
			//asyncNotifier_.data = this;
			//ev_async_start(loop, &asyncNotifier_);
			//on_notify_request(bind(ev_async_send, loop, &asyncNotifier_));

			SetSubscribeHandler(bind(&StatServerLibEv::OnSubscribeHandler, this, placeholders::_1, placeholders::_2));

			StatServer::Start(port);

		}

	protected:
		struct ev_loop* loop_;

		unordered_map<int,ev_io> socketWatchers_;
		static void OnRWSocketEvent_(struct ev_loop* loop, ev_io* w, int revents)
		{
			if (w->data)
			{
				if (revents & EV_READ)
					((StatServer *) w->data)->OnCanRead(w->fd);
				if (revents & EV_WRITE)
					((StatServer *) w->data)->OnCanWrite(w->fd);
			}
		}

		static void OnAcceptSocketEvent_(struct ev_loop* loop, ev_io* w, int revents)
		{
			if (w->data)
				((StatServer*)w->data)->OnCanAccept();
		}

		void OnSocketEvent(struct ev_loop* loop, ev_io* w, int revents){}

		ev_async asyncNotifier_;
		static void OnAsyncNotifier_(struct ev_loop* loop, ev_async* w, int revents){}
		void OnAsyncNotifier(struct ev_loop* loop, ev_async* w, int revents){}

		ev_timer timer_;
		static void OnTimer_(struct ev_loop* loop, ev_timer* w, int revents){}
		void OnTimer(struct ev_loop* loop, ev_timer* w, int revents){}


		void OnSubscribeHandler(int fd, int events)
		{
			if (events)
			{
				socketWatchers_[fd].data = this;
				auto& watcher = socketWatchers_[fd];

				if ((StatServer::SocketEvent) events == StatServer::SocketEvent::accept)
				{
					ev_init( &watcher, OnAcceptSocketEvent_);
					int evEvents = EV_READ;
					ev_io_set(&watcher, fd, evEvents);
					ev_io_start(loop_, &socketWatchers_[fd]);
                                } else
                                {
					ev_init( &watcher, OnRWSocketEvent_);
					int evEvents = (events & (int)StatServer::SocketEvent::read ? EV_READ  : EV_NONE)
								   | (events & (int)StatServer::SocketEvent::write ? EV_WRITE : EV_NONE);
					ev_io_set(&socketWatchers_[fd], fd, evEvents);
					ev_io_start(loop_, &socketWatchers_[fd]);
				}
			} else
			{
				auto it = socketWatchers_.find(fd);
				if (it != socketWatchers_.end())
				{
					if (ev_is_active(&it->second))
						ev_io_stop(loop_, &it->second);
					socketWatchers_.erase(it);
				}
			}
		}

	};

	class Metric
	{
	public:
		enum class Type
		{
			Counter
			,Gauge
			,Histogram
			,Summary
		};

		Metric(StatServer& server, string_view name, string_view description, Type type)
		: server_(&server)
		, name_(name)
		, type_(type)
		, description_(description)
		{
		}

		void AddLabel(string_view key,string_view value)
		{
			string _key;
			WriteEscape(_key, key);
			WriteEscape(labels_[_key], value);
		}

		void AddValue(double val)
		{
			value_ = val;

			auto &buf = server_->CurBuff();
                        if (buf.size() - buf.curPos_ < 0x1000)
                            buf.resize(buf.size() + 0x10000);
			// Write the header comments
			sprintf(buf.data() + buf.curPos_, "# TYPE %s %s\n", name_.c_str(), ToString(type_));
			buf.curPos_ += strlen(buf.data() + buf.curPos_);
			sprintf(buf.data() + buf.curPos_, "# HELP %s %s\n", name_.c_str(), description_.c_str());
			buf.curPos_ += strlen(buf.data() + buf.curPos_);

			memcpy(buf.data() + buf.curPos_, name_.c_str(), name_.length());
			buf.curPos_ += name_.length();

			// lables
			strcpy(buf.data() + buf.curPos_, "{");
			buf.curPos_ += strlen(buf.data() + buf.curPos_);

			for (auto &label : labels_)
			{
				strcpy(buf.data() + buf.curPos_, label.first.c_str());
				buf.curPos_ += label.first.length();
				strcpy(buf.data() + buf.curPos_, "=\"");
				buf.curPos_ += strlen(buf.data() + buf.curPos_);
				strcpy(buf.data() + buf.curPos_, label.second.c_str());
				buf.curPos_ += label.second.length();
				strcpy(buf.data() + buf.curPos_, "\",");
				buf.curPos_ += strlen(buf.data() + buf.curPos_);
			}
			if (labels_.size())
                                buf[buf.curPos_ - 1] = '}';
			else
			{
				strcpy(buf.data() + buf.curPos_, "}");
				buf.curPos_ += strlen(buf.data() + buf.curPos_);
			}
			//	value
			sprintf(buf.data() + buf.curPos_, " %f\n", val);
			buf.curPos_ += strlen(buf.data() + buf.curPos_);

		}


	protected:
		StatServer* server_;
		string name_;
		string description_;
		string value_;
		string labelKey_;
		string labelVal_;
		Type type_;
		unordered_map<string, string> labels_;


		const char* ToString(Type type)
		{
			switch (type)
			{
				case Type::Counter :
					return "counter";
				case Type::Gauge :
					return "gauge";
				case Type::Histogram :
					return "histogram";
				case Type::Summary :
					return "summary";
				default:
					return "undef";
			}

		}

		void WriteEscape(string& buf, string_view str)
		{
			for (char chr : str)
			{
				switch (chr)
				{
					case '\n':
						buf += "\\n";
						break;
					case '\\':
						buf += "\\\\";
						break;
					case '"':
						buf += "\\\"";
						break;
					default:
						buf += chr;
				}
			}
		}


	};

}
