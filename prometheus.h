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
#include <unordered_set>
#include <atomic>


using namespace std;

namespace Prometheus
{
		class OutBuff : public vector<char>
		{
		public:
				size_t curPos_ = 0;
		};


		class Metric
		{
		public:
				enum class Type
				{
						Counter
						, Gauge
						, Histogram
						, Summary
				};

				Metric(string_view name, string_view description, Type type)
								: name_(name)
									, type_(type)
									, description_(description)
				{
				}

				void AddLabel(string_view key, string_view value)
				{
					string _key;
					WriteEscape(_key, key);
					WriteEscape(labels_[_key], value);
				}

				void SetValue(double val)
				{
					if (type_ == Type::Counter)
					{
						double oldVal;
						do
						{
							double oldVal = cntVal_;
						} while (!cntVal_.compare_exchange_strong(oldVal, oldVal + val));
					}
					else
						value_ = val;
					ts_ = time(nullptr) * 1000;
				}

				void SetValue(string key, double val)
				{
					if (type_ == Type::Histogram)
					{
						double oldVal;
						do
						{
							double oldVal = bars_[key];
						} while (!bars_[key].compare_exchange_strong(oldVal, oldVal + val));
					}
					else
						bars_[key] = val;
					ts_ = time(nullptr) * 1000;
				}

				void Write(OutBuff& buf)
				{
					if (ts_ == 0)
						return;
					if ((type_ == Type::Summary || type_ == Type::Histogram) && bars_.size() == 0)
						return;
					if (buf.size() - buf.curPos_ < 0x1000)
						buf.resize(buf.size() + 0x10000);
					// Write the header comments
					sprintf(buf.data() + buf.curPos_, "# TYPE %s %s\n", name_.c_str(), ToString(type_));
					buf.curPos_ += strlen(buf.data() + buf.curPos_);
					sprintf(buf.data() + buf.curPos_, "# HELP %s %s\n", name_.c_str(), description_.c_str());
					buf.curPos_ += strlen(buf.data() + buf.curPos_);

					//	value
					if (type_ == Type::Summary || type_ == Type::Histogram)
					{
						auto bars = move(bars_);
						for (auto& bar : bars)
						{
							memcpy(buf.data() + buf.curPos_, name_.c_str(), name_.length());
							buf.curPos_ += name_.length();

							unordered_map<string, string> labels = labels_;
							labels[type_ == Type::Summary ? "quantile" : "le"] = bar.first;
							PrintLabels(buf, labels);
							sprintf(buf.data() + buf.curPos_, " %e %ld\n", bar.second.exchange(0), time(nullptr) * 1000);//ts_); prometheus don't like real ts
							buf.curPos_ += strlen(buf.data() + buf.curPos_);
						}
					}
					else
					{
						memcpy(buf.data() + buf.curPos_, name_.c_str(), name_.length());
						buf.curPos_ += name_.length();

						if (type_ == Type::Counter)
						{
							PrintLabels(buf, labels_);
							sprintf(buf.data() + buf.curPos_, " %e %ld\n", cntVal_.exchange(0), time(nullptr) * 1000);//ts_); prometheus don't like real ts
							buf.curPos_ += strlen(buf.data() + buf.curPos_);
						}
						else
						{
							PrintLabels(buf, labels_);
							sprintf(buf.data() + buf.curPos_, " %e %ld\n", value_, time(nullptr) * 1000);//ts_); prometheus don't like real ts
							buf.curPos_ += strlen(buf.data() + buf.curPos_);
						}
					}

				}


		protected:
				string name_;
				string description_;
				volatile double value_;
				atomic<double> cntVal_;
				Type type_;
				unordered_map<string, string> labels_;
				unordered_map<string, atomic<double>> bars_;
				long ts_ = 0;

				void PrintLabels(OutBuff& buf, unordered_map<string, string>& labels)
				{
					// lables
					strcpy(buf.data() + buf.curPos_, "{");
					buf.curPos_ += strlen(buf.data() + buf.curPos_);

					for (auto &label : labels)
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
					if (labels.size())
						buf[buf.curPos_ - 1] = '}';
					else
					{
						strcpy(buf.data() + buf.curPos_, "}");
						buf.curPos_ += strlen(buf.data() + buf.curPos_);
					}
				}

				const char *ToString(Type type)
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

				void WriteEscape(string &buf, string_view str)
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

		class StatServer
		{
		public:
				enum class SocketEvent
				{
						unsubscribe = 0
						, read = 1
						, write = 2
						, accept = 4
				};

				using OnSubscribeHandler = function<void(int fd, int events)>;

				StatServer()
				{
				}

				void Add(Metric *metric)
				{
					metrics_.insert(metric);
				}

				void Remove(Metric *metric)
				{
					metrics_.erase(metric);
				}

				void Start(int port)
				{
					StartListerning(port);
				}

				void SetSubscribeHandler(const OnSubscribeHandler &onSubscribeHandler)
				{
					onSubscribeHandler_ = onSubscribeHandler;
				}

				void OnCanAccept()
				{
					int newfd;
					struct sockaddr_storage their_addr;

					socklen_t sin_size = sizeof their_addr;
					newfd = accept(socketfd_, (struct sockaddr *) &their_addr, &sin_size);

					clients_.erase(newfd);
					auto &buff = clients_[newfd].buff_;
					buff.resize(0x10000);
					HttpWriteHeader(buff);
					for (auto metric : metrics_)
						metric->Write(buff);

					if (onSubscribeHandler_)
						onSubscribeHandler_(newfd, (int) SocketEvent::read);

				}

				void OnCanRead(int fd)
				{
					int status;
					char readBuffer[promReadBuffSize];
					do
					{
						status = recv(fd, readBuffer, sizeof readBuffer, MSG_DONTWAIT);
					} while (status > 0);

					OnCanWrite(fd);
				}

				void OnCanWrite(int fd)
				{
					auto clientData = clients_.find(fd);
					if (clientData == clients_.end())
					{
						if (onSubscribeHandler_)
							onSubscribeHandler_(fd, (int) SocketEvent::unsubscribe);
						shutdown(fd, 1);
						close(fd);
						return;
					}

					ssize_t writen = write(fd, clientData->second.buff_.data() + clientData->second.writePos_,
																 clientData->second.buff_.curPos_ - clientData->second.writePos_);
					if (writen <= 0 || writen == clientData->second.buff_.curPos_ - clientData->second.writePos_)
					{
						if (onSubscribeHandler_)
							onSubscribeHandler_(fd, (int) SocketEvent::unsubscribe);
						shutdown(fd, 1);
						close(fd);
						clients_.erase(fd);
					} else
						clientData->second.writePos_ += writen;
				}

		protected:
				class ClientData
				{
				public:
						OutBuff buff_;
						size_t writePos_ = 0;
				};

				static constexpr int promConnBacklog = 10;
				static constexpr size_t promReadBuffSize = 0x1000;

				unordered_map<int, ClientData> clients_;
				unordered_set<Metric *> metrics_;
				int socketfd_;
				OnSubscribeHandler onSubscribeHandler_;

				int StartListerning(int port)
				{
					int yes = 1;
					int gai_ret;
					struct addrinfo hints, *servinfo, *p;

					memset(&hints, 0, sizeof hints);
					hints.ai_family = AF_UNSPEC;
					hints.ai_socktype = SOCK_STREAM;
					hints.ai_flags = AI_PASSIVE;

					array<char, 10> port_str;
					sprintf(port_str.data(), "%d", port);

					gai_ret = getaddrinfo(NULL, port_str.data(), &hints, &servinfo);
					if (gai_ret != 0)
					{
						cerr << "getaddrinfo: " << gai_strerror(gai_ret) << endl;
						return -1;
					}

					for (p = servinfo; p != NULL; p = p->ai_next)
					{

						if ((socketfd_ = socket(p->ai_family, p->ai_socktype,
																		p->ai_protocol)) == -1)
						{
							continue;
						}

						if (setsockopt(socketfd_, SOL_SOCKET, SO_REUSEADDR, &yes,
													 sizeof(int)) == -1)
						{
							perror("setsockopt");
							close(socketfd_);
							freeaddrinfo(servinfo);
							return -2;
						}

						if (bind(socketfd_, p->ai_addr, p->ai_addrlen) == -1)
						{
							close(socketfd_);
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

					if (listen(socketfd_, promConnBacklog) == -1)
					{
						close(socketfd_);
						return -4;
					}

					if (onSubscribeHandler_)
						onSubscribeHandler_(socketfd_, (int) SocketEvent::accept);
				}

				void HttpWriteHeader(OutBuff &buf)
				{
					constexpr char header[] = "HTTP/1.1 200 OK\n\n";
					strcpy(buf.data(), header);
					buf.curPos_ += sizeof(header) - 1;
				}


		};

		class StatServerLibEv : public StatServer
		{
		public:
				void Start(struct ev_loop *loop, int port)
				{
					loop_ = loop;
					SetSubscribeHandler(bind(&StatServerLibEv::OnSubscribeHandler, this, placeholders::_1, placeholders::_2));
					StatServer::Start(port);
				}

		protected:
				struct ev_loop *loop_;

				unordered_map<int, ev_io> socketWatchers_;

				static void OnRWSocketEvent_(struct ev_loop *loop, ev_io *w, int revents)
				{
					if (w->data)
					{
						if (revents & EV_READ)
							((StatServer *) w->data)->OnCanRead(w->fd);
						if (revents & EV_WRITE)
							((StatServer *) w->data)->OnCanWrite(w->fd);
					}
				}

				static void OnAcceptSocketEvent_(struct ev_loop *loop, ev_io *w, int revents)
				{
					if (w->data)
						((StatServer *) w->data)->OnCanAccept();
				}

				void OnSocketEvent(struct ev_loop *loop, ev_io *w, int revents)
				{}

				ev_async asyncNotifier_;

				void OnSubscribeHandler(int fd, int events)
				{
					if (events)
					{
						socketWatchers_[fd].data = this;
						auto &watcher = socketWatchers_[fd];

						if ((StatServer::SocketEvent) events == StatServer::SocketEvent::accept)
						{
							ev_init(&watcher, OnAcceptSocketEvent_);
							int evEvents = EV_READ;
							ev_io_set(&watcher, fd, evEvents);
							ev_io_start(loop_, &socketWatchers_[fd]);
						} else
						{
							ev_init(&watcher, OnRWSocketEvent_);
							int evEvents = (events & (int) StatServer::SocketEvent::read ? EV_READ : EV_NONE)
														 | (events & (int) StatServer::SocketEvent::write ? EV_WRITE : EV_NONE);
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


}
