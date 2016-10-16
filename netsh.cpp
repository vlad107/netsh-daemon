#include <stdio.h>
#include <memory.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <string>
#include <iostream>
#include <set>
#include <map>
#include <vector>

#define LISTEN_BACKLOG 10
#define MAX_EVENTS 100
#define BUFF_SIZE 2048

void debug(int socket, std::string name) {
	std::cerr << name << ": " << socket << std::endl;
}

bool isquot(char c) {
	return ((c == '\'') || (c == '\"'));
}

std::set<int> write_fds;
std::map<int, int> last_fds;
std::vector<std::pair<int, int>> pipes;
int port, sfd, efd;
std::set<int> client_sockets;
std::map<int, int> who_writes;
std::map<int, int> wait_child;

struct my_data {
	my_data(int type) : type(type) {}
	int get_type() {
		return type;
	}
private:
	int type;
};

void epoll_add(int efd, int fd, int mask); 
void epoll_remove(int efd, int fd, int mask);

void error(std::string s) {
	std::cerr << s << std::endl;
	exit(1);
}

struct Buffer {
	Buffer() : s(""), cur(0), lq(0), _execed(false), id_endl(-1), write_fd(-1), client_fd(-1) {}
	void add_data(std::string ns) {
		if (ns == "") return;
		int prev_len = (int)s.size();
		s += ns;
		if (id_endl == -1) {
			for (int i = prev_len; i < (int)s.size(); i++) {
				if (lq != 0) {
					if (s[i] == lq) {
						lq = 0;
						continue;
					}
				} else if (isquot(s[i])) {
					lq = s[i];
				} else if (s[i] == '\n') {
					id_endl = i;
					break;
				}
			}
		}
	}

	std::string get_command() {
		cur = id_endl + 1;
		return s.substr(0, id_endl);
	}

	void write_chunk() {
		if (cur == (int)s.size()) return;
		char buff[BUFF_SIZE];
		int len = std::min((int)s.size() - cur, BUFF_SIZE);	
		for (int i = 0; i < len; i++) buff[i] = s[i + cur];
		buff[len] = 0;
		int nw;
		while (1) {
			nw = ::write(write_fd, buff, len);		
			if ((nw < 0) && (errno == EINTR)) continue;
			break;
		}
		if (nw < 0) error("Error in write():\n" + std::string(strerror(errno)));
		cur += nw;
		if (cur == (int)s.size()) {
			auto it = write_fds.find(write_fd);
			if (it != write_fds.end()) {
				write_fds.erase(it);
				epoll_remove(efd, write_fd, EPOLLOUT);
			}
		}
	}

	void flush() {
		write_chunk();
		return;


		if (cur == (int)s.size()) return;
		if (write_fds.find(write_fd) == write_fds.end()) {
			write_fds.insert(write_fd);
			std::cerr << "in flush fd = " << write_fd << std::endl;
			epoll_add(efd, write_fd, EPOLLOUT);
		}
	}
	void set_write_fd(int fd) {
		write_fd = fd;
		who_writes[fd] = client_fd;
	}
	void make_execed() { _execed = true; }
	void set_client_fd(int fd) { client_fd = fd; }
	bool execed() { return _execed; }
	bool was_endl() { return id_endl != -1; }
private:
	std::string s;
	int cur;
	char lq;
	bool _execed;
	int id_endl;
	int write_fd;
	int client_fd;
};

std::map<int, int> first_fd;
std::map<int, Buffer> cl_buff;

std::vector<std::string> split(std::string s, char sep) {
	s += sep;
	std::vector<std::string> result;
	char lq = 0;
	std::string cur;
	for (int i = 0; i < (int)s.size(); i++) {
		if (lq != 0) {
			cur += s[i];
			if (s[i] == lq) {
				lq = 0;
				continue;
			}
		} else if (isquot(s[i])) {
			lq = s[i];
			cur += s[i];
		} else if (s[i] == sep) {
			if (cur != "") result.push_back(cur);
			cur = "";
		} else cur += s[i];
	}
	return result;
}

std::string read_all(int fd) {
	char buff[BUFF_SIZE];
	int nr;
	std::string result;
	while ((nr = read(fd, buff, BUFF_SIZE)) != 0) {
		if (nr < 0) {
			if (errno == EINTR) continue;
			break;
		}	
		result += std::string(buff);
	}
	if (nr < 0) {
		if (errno != EAGAIN) {
			error("Error in read_all():\n" + std::string(strerror(errno)));
		}
	}
		
	return result;
}

void make_nonblocking(int fd) {
	int flags;
	if ((flags = fcntl(fd, F_GETFL, 0)) < 0) error("Error in fcntl() (F_GETFL)");
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) error("Error in fcntl() (F_SETFL)");
}

int create_socket(int port) {
	int sfd;
	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) error("Error in socket()");
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sfd, (struct sockaddr*) &addr, sizeof(struct sockaddr)) < 0) error("Error in bind()");
	if (listen(sfd, LISTEN_BACKLOG)) error("Error in listen()");
	return sfd;
}

int create_epoll() {
	int efd;
	if ((efd = epoll_create1(0)) < 0) error("Error in create_epoll()");
	return efd;
}

void epoll_add(int efd, int fd, int mask) {
	epoll_event ev;
	memset(&ev, 0, sizeof(epoll_event));
	ev.data.fd = fd;
	ev.events = mask;
	std::cerr << "epoll_add: fd = " << fd << std::endl;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) < 0) {
//		error(strerror(errno));
		error("Error in epoll_ctl() (EPOLL_CTL_ADD)");
	}	
}

void epoll_remove(int efd, int fd, int mask) {
	epoll_event ev;
	memset(&ev, 0, sizeof(epoll_event));
	ev.data.fd = fd;
	ev.events = mask;
	std::cerr << "epoll_remove: fd = " << fd << std::endl;
	if (epoll_ctl(efd, EPOLL_CTL_DEL, fd, &ev) < 0) {
//		error(strerror(errno));
		error("Error in epoll_ctl() (EPOLL_CTL_DEL)");
	}
	close(fd);
}

void accept_handler(int sfd) {
	sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int cfd;
	while (1) {
		cfd = accept(sfd, (sockaddr*)&addr, &addr_len);
		if ((cfd < 0) && (errno == EINTR)) continue;
		break;
	}
	if (cfd < 0) error("Error in accept():\n" + std::string(strerror(errno)));
	debug(cfd, "New client to socket");
	make_nonblocking(cfd);
	epoll_add(efd, cfd, EPOLLIN);
	client_sockets.insert(cfd);
}

int exec_command(std::string command, int curstdin, int curstdout) {
	int pid = fork();
	if (pid < 0) error("Error in fork():\n" + std::string(strerror(errno)));
	if (pid == 0) {
		close(STDIN_FILENO); dup(curstdin);
		close(STDOUT_FILENO); dup(curstdout);
		close(curstdin);
		close(curstdout);
		std::vector<std::string> args = split(command, ' ');
		char **argv = new char*[args.size() + 1];
		for (int i = 0; i < (int)args.size(); i++) {
			argv[i] = new char[args[i].size() + 1];
			strcpy(argv[i], args[i].c_str());
			argv[i][(int)args[i].size()] = 0;
		}
		argv[(int)args.size()] = 0;
		execvp(argv[0], argv);
		exit(0);
	}
	return pid;
}

void client_handler(int cfd) {
	std::string s = read_all(cfd);
	Buffer &buff = cl_buff[cfd];
	buff.set_client_fd(cfd);
	buff.add_data(s);
	if (buff.execed()) {
		buff.flush();
	} else if (buff.was_endl()) {
		buff.make_execed();
		std::string command = buff.get_command();		
		std::vector<std::string> commands = split(command, '|');
		int p[2];
		pipe2(p, O_CLOEXEC);
		buff.set_write_fd(p[1]);
		int curstdin = p[0];
		for (int i = 0; i < (int)commands.size(); i++) {
			int nextstdin, curstdout;
			if (i + 1 < (int)commands.size()) {
				pipe2(p, O_CLOEXEC);
				nextstdin = p[0];
				curstdout = p[1];
			} else {
				nextstdin = -1;
				curstdout = cfd;
			}
			int chpid = exec_command(commands[i], curstdin, curstdout);
			close(curstdin);
			if (i + 1 < (int)commands.size()) {
				close(curstdout);
			} else wait_child[chpid] = cfd;
			curstdin = nextstdin;
		}
		buff.flush();
	}
}

void chld_handler(int signum, siginfo_t *info, void*) {
	assert(signum == SIGCHLD);
	int pid = info->si_pid;
	waitpid(pid, NULL, 0);
	if (wait_child.count(pid) > 0) {
		int cfd = wait_child[pid];
		epoll_remove(efd, cfd, EPOLLIN);
		close(cfd);
		wait_child.erase(pid);
	}
}

void make_sigact() {
	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = chld_handler;
	if (sigaction(SIGCHLD, &sa, NULL) < 0) error("Error in sigaction()");
}

int main(int argc, char *argv[]) {
	if ((argc != 2) || (sscanf(argv[1], "%d\n", &port) < 0)) error("Usage: " + std::string(argv[0]) + " [port]");
	make_sigact();
	sfd = create_socket(port);
	debug(sfd, "main socket");
	make_nonblocking(sfd);
	efd = create_epoll();
	debug(efd, "epoll socket");
	epoll_add(efd, sfd, EPOLLIN);
	struct epoll_event evs[MAX_EVENTS];
	while (true) {
		int ev_size;
		if ((ev_size = epoll_wait(efd, evs, MAX_EVENTS, -1)) < 0) {
			error("Error in epoll_wait:\n" + std::string(strerror(errno)));
		}
		for (int i = 0; i < ev_size; i++) {
			int fd = evs[i].data.fd;
			if (fd == sfd) {
				accept_handler(sfd);
			} else if (client_sockets.find(fd) != client_sockets.end()) {
				client_handler(fd);
			} else if (write_fds.find(fd) != write_fds.end()) {
				int cfd = who_writes[fd];
				Buffer &buff = cl_buff[cfd];
				buff.write_chunk();
			}
		}
	}
}

