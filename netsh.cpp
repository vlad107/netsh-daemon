#include <stdio.h>
#include <memory.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#include <sys/stat.h>
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

#define deb(x) std::cerr << x << std::endl
#define LISTEN_BACKLOG 10
#define MAX_EVENTS 100
#define BUFF_SIZE 1024 * 10
#define BIG_BUFF_SIZE 1024 * 100 

void debug(int socket, std::string name) {
	std::cerr << name << ": " << socket << std::endl;
}

bool isquot(char c) {
	return ((c == '\'') || (c == '\"'));
}

std::set<int> write_fds;
int port, sfd, efd;
std::set<int> client_sockets;
std::map<int, int> who_writes;
std::map<int, int> wait_child;

void epoll_add(int efd, int fd, int mask); 
void epoll_remove(int efd, int fd, int mask);

void error(std::string s) {
	std::cerr << s << std::endl;
	exit(1);
}

struct Buffer {
	Buffer() : s(""), lq(0), _execed(false), id_endl(-1), write_fd(-1), client_fd(-1), write_fd_closed(false), cfd(-1),_removed(false) {}
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
		if (s.size() > BIG_BUFF_SIZE) {
			deb("1");
			remove_from_epoll();
		}
	}

	std::string get_command() {
		std::string result = s.substr(0, id_endl);
		s = s.substr(id_endl);
		return result;
	}

	void write_chunk() {
		bool in_epoll = (int)s.size() <= BIG_BUFF_SIZE;
		if (s.empty()) return;
//		std::cerr << "cur = " << cur << " / " << s.size() << std::endl;
		char buff[BUFF_SIZE];
		int len = std::min((int)s.size(), BUFF_SIZE);	
		for (int i = 0; i < len; i++) buff[i] = s[i];
		buff[len] = 0;
//		fprintf(stderr, "Submit %s for %s\n", buff, s.c_str());
		int nw;
		while (1) {
			nw = ::write(write_fd, buff, len);		
			std::cerr << "written " << nw << " chars to " << write_fd << " descriptor" << std::endl;
			if ((nw < 0) && (errno == EINTR)) continue;
			break;
		}
		if (nw < 0) error("Error in write():\n" + std::string(strerror(errno)));
		s = s.substr(nw);
		if (s.empty()) {
			auto it = write_fds.find(write_fd);
			if (it != write_fds.end()) {
				write_fds.erase(it);
				deb("3");
				epoll_remove(efd, write_fd, EPOLLOUT);
			}
			if (write_fd_closed) close(write_fd);
		}
		if ((s.size() <= BIG_BUFF_SIZE) && (!in_epoll)) {
			epoll_add(efd, cfd, EPOLLIN);
			_removed = false;
		}
	}

	void flush() {
		if (s.empty()) return;
		if (write_fds.find(write_fd) == write_fds.end()) {
			write_fds.insert(write_fd);
			epoll_add(efd, write_fd, EPOLLOUT);
//			std::cerr << "Socket for writing: " << write_fd << std::endl;
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
	int get_write_fd() { return write_fd; }
	void close_write_fd() { 
		write_fd_closed = true; 
		if (0 == (int)s.size()) close(write_fd);
	}
	bool closed() { return write_fd_closed; }
	void set_cfd(int _cfd) { cfd = _cfd; }
	bool in_epoll() { return !_removed; }
	void remove_from_epoll() { 
		if (_removed) return;
		epoll_remove(efd, cfd, EPOLLIN);
		_removed = true; 
	}
private:
	std::string s;
	char lq;
	bool _execed;
	int id_endl;
	int write_fd;
	int client_fd;
	bool write_fd_closed;
	int cfd;
	bool _removed;
};

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
		buff[nr] = 0;
		result += std::string(buff, nr);
	}
	if (nr < 0) {
		if (errno != EAGAIN) {
			error("Error in read_all(): " + std::to_string(fd) + "\n" + std::string(strerror(errno)));
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
	int ok = 1;
	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &ok, sizeof(ok)) < 0) error("setsockopt");
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
		error("Error in epoll_ctl() (EPOLL_CTL_DEL)" + std::string(strerror(errno)));
	}
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
	//make_nonblocking(cfd);
	epoll_add(efd, cfd, EPOLLIN);
	client_sockets.insert(cfd);
}

int exec_command(std::string command, int curstdin, int curstdout, int cfd) {
	int pid = fork();
	if (pid < 0) error("Error in fork():\n" + std::string(strerror(errno)));
	if (pid == 0) {
		close(STDIN_FILENO); dup(curstdin);
		close(STDOUT_FILENO); dup(curstdout);
		close(curstdin);
		close(curstdout);
		close(cfd);
		std::vector<std::string> args = split(command, ' ');
		char **argv = new char*[args.size() + 1];
		for (int i = 0; i < (int)args.size(); i++) {
			if (isquot(args[i][0]) && (args[i][0] == args[i][(int)args[i].size() - 1])) {
				args[i] = args[i].substr(1, (int)args[i].size() - 2);
			}
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
	buff.set_cfd(cfd);
	if (s == "") {
		std::cerr << "Im here" << std::endl;
		if (!buff.execed()) error("Was not execed");
		deb("2");
		buff.remove_from_epoll();
		buff.close_write_fd();
		return;
	}
	buff.set_client_fd(cfd);
	buff.add_data(s);
	if (buff.execed()) {
		//std::cerr << "flush socket:" << cfd << std::endl;
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
			int chpid = exec_command(commands[i], curstdin, curstdout, cfd);
			std::cerr << "\"" << commands[i] << "\"" << " is executing in " << chpid << " process" << std::endl;
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
	std::cerr << pid << " has been closed" << std::endl; 
	if (wait_child.count(pid) > 0) {
		int cfd = wait_child[pid];
		if (!cl_buff[cfd].closed()) {
			close(cl_buff[cfd].get_write_fd());
			deb("4");
			cl_buff[cfd].remove_from_epoll();
		}
		close(cfd);
		client_sockets.erase(cfd);
		wait_child.erase(pid);
		cl_buff.erase(cfd);
	}
}

void make_sigact() {
	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = chld_handler;
	if (sigaction(SIGCHLD, &sa, NULL) < 0) error("Error in sigaction()");
}

void become_daemon() {
	int pid = fork();
	if (pid < 0) error("Error in fork():\n" + std::string(strerror(errno)));
	if (pid != 0) exit(0);
	setsid();
	pid = fork();
	if (pid < 0) error("Error in fork():\n" + std::string(strerror(errno)));
	if (pid != 0) exit(0);
	pid = getpid();
	chdir("/");
	umask(0);
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	int fd = open("/tmp/netsh.pid", O_WRONLY | O_CREAT);
	if (fd < 0) error("Error in open():\n" + std::string(strerror(errno)));
	pid = getpid();
	dprintf(fd, "%d\n", pid);
	close(fd);
}

int main(int argc, char *argv[]) {
	if ((argc != 2) || (sscanf(argv[1], "%d\n", &port) < 0)) error("Usage: " + std::string(argv[0]) + " [port]");
//	become_daemon();
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
			if (errno == EINTR) continue;
			error("Error in epoll_wait:\n" + std::string(strerror(errno)));
		}
		for (int i = 0; i < ev_size; i++) {
			int fd = evs[i].data.fd;
			std::cerr << "events = " << evs[i].events << std::endl;
			if (evs[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR | EPOLLET)) {
				error("Error in epoll_event");
				continue;
			}
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

