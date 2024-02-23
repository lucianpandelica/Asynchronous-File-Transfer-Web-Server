#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <errno.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>

#include "aws.h"
#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"

/* file descriptor pentru socketul serverului */
static int listenfd;

/* file descriptor pentru epoll */
static int epollfd;

/* parser HTTP */
static http_parser request_parser;

/* final HTTP request */
static char end_of_req[5] = "\r\n\r\n\0";

/* final buffer de receive */
static char end_of_buf[5];

/* nume folder "dynamic" */
static char folderDynamic[8] = "dynamic";

/* adresa curenta a vectorului req_path de procesat */
static char *crt_path_addr;

/* structura pereche (fd, conn) */
struct conn_fd_map {
	int fd;
	struct connection *conn_addr;
};

/* vector de perechi (fd, conn) */
static struct conn_fd_map *conn_array;

/* numar elemente vector perechi */
static int num_conn;

enum connection_state {
	STATE_DATA_PARTIAL_RECEIVED,
	STATE_DATA_RECEIVED,
	STATE_HEADER_PARTIAL_SENT,
	STATE_HEADER_SENT,
	STATE_DATA_PARTIAL_SENT,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

enum async_io_state {
	STATE_NOT_READY,
	STATE_IN_PROGRESS,
	STATE_READY
};

/* handler conexiune */
struct connection {

	/* file descriptor socket asociat */
	int sockfd;

	/* buffer receive */
	char recv_buffer[BUFSIZ];
	size_t recv_len;

	/* status conexiune */
	enum connection_state state;

	/* comanda request */
	char request_command[BUFSIZ];

	/* calea catre fisier din comanda clientului */
	char request_path[BUFSIZ];

	/* file descriptor al fisierului cerut */
	int req_fd;

	/* dimensiune fisier cerut */
	int file_size;

	/* header */
	char header[BUFSIZ];

	/* bytes ramasi de trimis din header */
	int left_bytes_header;

	/* bytes ramasi de trimis din fisier */
	int left_bytes_file;

	/* bytes ramasi de trimis din revc_buffer */
	int left_bytes_buffer;

	/* error-404 check */
	int error_404;

	/* -- pentru operatii I/O -- */

	/* variabila io_context */
	io_context_t ctx;

	/* file descriptor eventfd */
	int ev_fd;

	/* dynamic file check */
	int is_dyn;

	/* status operatie AIO */
	enum async_io_state aio_state;
};

/* -- parser HTTP -- */

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */
static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(crt_path_addr, buf, len);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/* submit unei noi citiri asincrone din fisier */
static void submit_file_read(struct connection *conn)
{
	struct iocb *new_iocb;

	memset(conn->recv_buffer, 0, BUFSIZ);

	/* construim structura iocb asociata */
	new_iocb = (struct iocb *) malloc(sizeof(struct iocb));
	new_iocb->data = conn;
	new_iocb->aio_lio_opcode = IO_CMD_PREAD;
	new_iocb->aio_fildes = conn->req_fd;
	new_iocb->u.c.buf = conn->recv_buffer;
	new_iocb->u.c.nbytes = BUFSIZ;
	new_iocb->u.c.offset = conn->file_size - conn->left_bytes_file;

	/* trimitem cererea de citire */
	io_submit(conn->ctx, 1, &new_iocb);
	conn->aio_state = STATE_NOT_READY;
}

/* adauga o intrare la vectorul de perechi (fd, conn) */
static void add_conn(int fd, struct connection *conn)
{
	conn_array = realloc(conn_array,
						 (num_conn + 1) * sizeof(struct conn_fd_map));

	conn_array[num_conn].fd = fd;
	conn_array[num_conn].conn_addr = conn;

	num_conn++;
}

/* marcheaza eliminarea unei intrari din vectorul de perechi (fd, conn) */
static void remove_conn(int fd)
{
	int i = 0;

	for (i = 0; i < num_conn; i++) {
		if (conn_array[i].fd == fd && conn_array[i].conn_addr != NULL) {
			/* setam valori invalide pentru element */
			conn_array[i].fd = -1;
			conn_array[i].conn_addr = NULL;

			return;
		}
	}
}

/*
 * intoarce conexiunea asociata unui anumit fd din vectorul de perechi (fd, conn)
 */
static struct connection *get_conn(int fd)
{
	int i = 0;

	for (i = 0; i < num_conn; i++) {
		if (conn_array[i].fd == fd)
			return conn_array[i].conn_addr;
	}

	return NULL;
}

/* initializeaza atributele unei conexiuni */
static void init_conn_data(struct connection *conn)
{

	memset(conn->request_command, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	conn->req_fd = -1;
	conn->error_404 = 0;
	conn->ctx = 0;
}

/* construieste header-ul */
static void build_header(struct connection *conn)
{
	char string_size[10];

	memset(conn->header, 0, BUFSIZ);
	strcat(conn->header, "HTTP/1.0 200 OK\r\nContent-Length: ");

	sprintf(string_size, "%d", conn->file_size);

	strcat(conn->header, string_size);
	strcat(conn->header, "\r\nConnection: close\r\n\r\n");
}

/* deschide fisierul cerut */
static int open_req_file(struct connection *conn)
{
	int rc = 0;
	struct stat file_stat;
	int len_path = strlen(conn->request_path);
	char *path = (char *) malloc((2 + len_path) * sizeof(char));

	/* compunem calea catre fisier */
	memset(path, 0, (2 + len_path));
	memcpy(path, AWS_DOCUMENT_ROOT, (strlen(AWS_DOCUMENT_ROOT) - 1));
	strcat(path, conn->request_path);

	/* deschidem fisierul */
	conn->req_fd = open(path, O_RDONLY);
	if (conn->req_fd == -1)
		return -1;

	/* ii aflam dimensiunea */
	rc = stat(path, &file_stat);
	DIE(rc < 0, "stat");
	free(path);

	/* golim bufferul de receive */
	memset(conn->request_path, 0, BUFSIZ);

	return file_stat.st_size;
}

/* preia ultimele caractere din bufferul curent */
static void get_last_char(char recv_buff[])
{
	int i = 0;
	int len = 0;

	len = strlen(recv_buff);
	for (i = (len - 4); i <= (len - 1); i++)
		end_of_buf[i - (len - 4)] = recv_buff[i];
}

/* parseaza continutul request_command pentru a determina request_path */
static void get_path(struct connection *conn)
{
	size_t bytes_parsed;

	http_parser_init(&request_parser, HTTP_REQUEST);
	crt_path_addr = conn->request_path;
	bytes_parsed = http_parser_execute(&request_parser,
									   &settings_on_path,
									   conn->request_command,
									   strlen(conn->request_command));
	if (bytes_parsed < 0)
		perror("Error: parser");

	/* stergem vechea comanda */
	memset(conn->request_command, 0, BUFSIZ);
}

/*
 * functie ce creeaza structura unei conexiuni
 */
static struct connection *connection_create(int sockfd)
{
	struct connection *conn = (struct connection *)
							  malloc(sizeof(struct connection));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;

	/* marcam socket-ul ca non-blocant */
	fcntl(conn->sockfd, F_SETFL, fcntl(conn->sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* initializam bufferul de receive */
	memset(conn->recv_buffer, 0, BUFSIZ);

	return conn;
}

/*
 * functie ce inchide conexiunea si ii elibereaza resursele
 */
static void close_connection(struct connection *conn)
{
	int rc;

	if (conn->is_dyn == 1) {

		remove_conn(conn->ev_fd);

		rc = close(conn->ev_fd);
		DIE(rc < 0, "close");

		rc = io_destroy(conn->ctx);
		DIE(rc != 0, "io_destroy");
	}

	rc = close(conn->sockfd);
	DIE(rc < 0, "close");

	free(conn);
}

/*
 * functie ce elimina o conexiune
 */
static enum connection_state remove_connection(struct connection *conn)
{
	int rc = 0;

	/* eliminam din lista de interese */
	rc = w_epoll_remove_fd(epollfd, conn->sockfd);
	DIE(rc < 0, "w_epoll_remove_fd");

	/* eliminam perechea coresp lui sockfd din vector */
	remove_conn(conn->sockfd);

	/* eliberam toate resursele conexiunii si o inchidem */
	close_connection(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * functie ce se ocupa de o noua conexiune venita pe socketul serverului
 */
static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* acceptam noua conexiune */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	/* structura aferenta noii conexiuni */
	conn = connection_create(sockfd);

	/* initializam campurile conexiunii */
	init_conn_data(conn);

	/* adaugam la vectorul de perechi fd - conexiune */
	add_conn(sockfd, conn);

	/* adaugam sockfd la epoll pentru evenimente de IN */
	rc = w_epoll_add_fd_in(epollfd, sockfd);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * functie ce se ocupa de citirea cererii clientului si de aflarea informatiilor
 * necesare deciziei asupra parcursului ulterior al trimiterii raspunsului
 */
static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;

	/* reinitializam bufferul de recv si citim */
	memset(conn->recv_buffer, 0, BUFSIZ);
	bytes_recv = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);

	if (bytes_recv < 0) {
		perror("Receive: Error in communication");
		return remove_connection(conn);
	} else if (bytes_recv == 0 && conn->state == STATE_DATA_SENT) {
		/* connection closed */
		return remove_connection(conn);
	} else if (bytes_recv == 0 && conn->state != STATE_DATA_SENT) {
		perror("Receive: Waiting to finish sending to client");
		return conn->state;
	}

	/*
	 * atasam continutul curent al bufferului
	 * de receive la comanda de request
	 */
	strcat(conn->request_command, conn->recv_buffer);

	/* extragem ultimele caractere din continutul actual al comenzii de request */
	get_last_char(conn->request_command);

	/*
	 * verificam daca s-au citit ultimii octeti
	 * din comanda primita la tastatura
	 */
	if (strcmp(end_of_buf, end_of_req) != 0) {
		/* in caz negativ, mai avem octeti de citit */
		conn->state = STATE_DATA_PARTIAL_RECEIVED;
		return STATE_DATA_PARTIAL_RECEIVED;
	}

	/* in caz afirmativ, obtinem calea fisierului din comanda primita */
	get_path(conn);

	/* determinam tipul fisierului si il retinem */
	if (strncmp(conn->request_path + 1, folderDynamic, 7) == 0) {

		conn->is_dyn = 1;

		/* cream contextul I/O asincron pentru conexiunea curenta */
		rc = io_setup(1, &(conn->ctx));
		DIE(rc != 0, "io_setup");
	} else {
		conn->is_dyn = 0;
	}

	/* marcam faptul ca am primit toata comanda */
	conn->state = STATE_DATA_RECEIVED;
	return STATE_DATA_RECEIVED;
}

/*
 * functie ce trimite un fisier dinamic catre client
 */
static enum connection_state send_file_dyn(struct connection *conn)
{
	int rc;
	ssize_t bytes_sent;

	if (conn->state == STATE_HEADER_SENT ||
		conn->state == STATE_DATA_PARTIAL_SENT) {

		if (conn->aio_state == STATE_READY) {

			int offset = conn->recv_len - conn->left_bytes_buffer;

			bytes_sent = send(conn->sockfd,
							  conn->recv_buffer + offset,
							  conn->left_bytes_buffer,
							  0);

			if (bytes_sent < 0) {
				perror("Send: Error in communication.");
				return remove_connection(conn);
			}
			if (bytes_sent < conn->left_bytes_buffer) {

				/* nu am terminat scrierea */
				conn->left_bytes_buffer = conn->left_bytes_buffer - bytes_sent;

				conn->state = STATE_DATA_PARTIAL_SENT;
				return STATE_DATA_PARTIAL_SENT;
			}

			/* actualizam numarul de bytes de trimis din fisier */
			conn->left_bytes_file = conn->left_bytes_file - conn->recv_len;

			if (conn->left_bytes_file != 0) {

				/* cerem o noua citire */
				submit_file_read(conn);

				/* adaugam evenimentul asociat operatiei la epoll */
				rc = w_epoll_add_fd_out(epollfd, conn->ev_fd);
				DIE(rc < 0, "w_epoll_add_fd_out");

				conn->state = STATE_DATA_PARTIAL_SENT;
				return STATE_DATA_PARTIAL_SENT;
			}

			/* am terminat trimiterea datelor, evenimentul asociat devine de IN */
			rc = w_epoll_update_fd_in(epollfd, conn->sockfd);
			DIE(rc < 0, "w_epoll_update_fd_in");

			conn->state = STATE_DATA_SENT;
			return STATE_DATA_SENT;
		}
	}

	return conn->state;
}

/*
 * functie ce trimite header-ul corespunzator unui fisier dinamic catre un client
 */
static enum connection_state send_header(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;

	if (conn->state == STATE_DATA_RECEIVED) {

		conn->file_size = open_req_file(conn);

		if (conn->file_size == -1) {
			memset(conn->header, 0, BUFSIZ);
			strcat(conn->header, "HTTP/1.1 404 Not Found\r\n\r\n");
			conn->error_404 = 1;
		} else {
			build_header(conn);
		}

		conn->left_bytes_header = strlen(conn->header);
		conn->left_bytes_file = conn->file_size;
	}

	if (conn->state == STATE_DATA_RECEIVED ||
		conn->state == STATE_HEADER_PARTIAL_SENT) {

		int offset_header = strlen(conn->header) - conn->left_bytes_header;

		bytes_sent = send(conn->sockfd,
						  conn->header + offset_header,
						  conn->left_bytes_header,
						  0);

		if (bytes_sent < 0) {
			perror("Send: Error in communication.");
			return remove_connection(conn);
		}
		if (bytes_sent < conn->left_bytes_header) {

			/* nu am terminat scrierea */
			conn->left_bytes_header = conn->left_bytes_header - bytes_sent;

			conn->state = STATE_HEADER_PARTIAL_SENT;
			return STATE_HEADER_PARTIAL_SENT;
		}

		if (conn->error_404 == 1) {

			/* am incheiat trimiterea datelor */
			rc = w_epoll_update_fd_in(epollfd, conn->sockfd);
			DIE(rc < 0, "w_epoll_update_fd_in");

			conn->state = STATE_DATA_SENT;
			return STATE_DATA_SENT;
		}

		/* cerem o noua citire */
		submit_file_read(conn);

		/* adaugam evenimentul asociat operatiei la epoll */
		rc = w_epoll_add_fd_out(epollfd, conn->ev_fd);
		DIE(rc < 0, "w_epoll_add_fd_out");

		/* am terminat scrierea header-ului */
		conn->state = STATE_HEADER_SENT;
		return STATE_HEADER_SENT;
	}

	return conn->state;
}

/*
 * functie ce trimite raspunsul catre un client ce a cerut un fisier static
 */
static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;

	/* daca intram in send imediat dupa primirea comenzii */
	if (conn->state == STATE_DATA_RECEIVED) {

		/* deschidem fisierul cerut si ii aflam dimensiunea */
		conn->file_size = open_req_file(conn);

		/* verificam daca s-a gasit fisierul */
		if (conn->file_size == -1) {

			/* in caz negativ, construim un header HTTP 404 */
			memset(conn->header, 0, BUFSIZ);
			strcat(conn->header, "HTTP/1.1 404 Not Found\r\n\r\n");

			conn->error_404 = 1;
		} else {

			/* altfel, construim un header HTTP 200 */
			build_header(conn);
		}

		/* retinem cati octeti avem de trimis */
		conn->left_bytes_header = strlen(conn->header);
		conn->left_bytes_file = conn->file_size;
	}

	/* daca nu am trimis header-ul / l-am trimis partial */
	if (conn->state == STATE_DATA_RECEIVED ||
		conn->state == STATE_HEADER_PARTIAL_SENT) {

		/* calculam offset-ul curent in header */
		int offset_header = strlen(conn->header) - conn->left_bytes_header;

		bytes_sent = send(conn->sockfd,
						  conn->header + offset_header,
						  conn->left_bytes_header,
						  0);

		if (bytes_sent < 0) {
			perror("Send: Error in communication.");
			return remove_connection(conn);
		}
		if (bytes_sent == 0) {
			/* connection closed */
			return remove_connection(conn);
		}
		if (bytes_sent < conn->left_bytes_header) {

			/*
			 * nu am terminat scrierea header-ului,
			 * actualizam numarul de octeti ramasi
			 */
			conn->left_bytes_header = conn->left_bytes_header - bytes_sent;

			conn->state = STATE_HEADER_PARTIAL_SENT;
			return STATE_HEADER_PARTIAL_SENT;
		}

		/* am terminat scrierea header-ului */
		conn->state = STATE_HEADER_SENT;
		return STATE_HEADER_SENT;
	}

	/*
	 * daca am trimis header-ul si nu s-a gasit
	 * fisierul pentru conexiunea curenta
	 */
	if (conn->error_404 == 1) {

		/*
		 * am terminat trimiterea datelor,
		 * actualizam socket-ul pentru evenimente de IN
		 */
		rc = w_epoll_update_fd_in(epollfd, conn->sockfd);
		DIE(rc < 0, "w_epoll_update_fd_in");

		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;
	}

	/* daca am trimis header-ul si nu am trimis fisierul / l-am trimis partial */
	if (conn->state == STATE_HEADER_SENT ||
		conn->state == STATE_DATA_PARTIAL_SENT) {

		bytes_sent = sendfile(conn->sockfd,
							  conn->req_fd,
							  NULL,
							  conn->left_bytes_file);

		if (bytes_sent < 0) {
			perror("Send: Error in communication.");
			return remove_connection(conn);
		}
		if (bytes_sent == 0) {
			/* communication closed */
			return remove_connection(conn);
		}
		if (bytes_sent < conn->left_bytes_file) {

			/*
			 * nu am terminat scrierea fisierului,
			 * actualizam numarul de octeti ramasi
			 */
			conn->left_bytes_file = conn->left_bytes_file - bytes_sent;

			conn->state = STATE_DATA_PARTIAL_SENT;
			return STATE_DATA_PARTIAL_SENT;
		}

		/*
		 * am terminat trimiterea fisierului,
		 * actualizam socket-ul pentru evenimente de IN
		 */
		rc = w_epoll_update_fd_in(epollfd, conn->sockfd);
		DIE(rc < 0, "w_epoll_update_fd_in");

		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;
	}

	return conn->state;
}

/*
 * functie care se ocupa de cererea clientului
 */
static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	/* citim urmatoarea parte din request */
	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	/* daca am citit tot request-ul */
	if (ret_state == STATE_DATA_RECEIVED) {

		if (conn->is_dyn == 1) {

			conn->ev_fd = eventfd(0, 0);
			add_conn(conn->ev_fd, conn);
		}

		/* adaugam socket-ul la epoll pentru evenimente de OUT */
		rc = w_epoll_update_fd_out(epollfd, conn->sockfd);
		DIE(rc < 0, "w_epoll_update_fd_out");
	}
}

int main(void)
{
	int rc;

	/*
	 * initializam numarul de conexiuni primite
	 * de server de la pornirea acestuia la 0
	 */
	num_conn = 0;

	/* cream instanta epoll */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* cream socketul serverului */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	/* adaugam socketul la lista de interese a epoll pentru evenimente de IN */
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	while (1) {

		struct epoll_event event;

		/* ateptam un nou eveniment */
		rc = w_epoll_wait_infinite(epollfd, &event);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* am primit un eveniment, verificam daca este o noua conexiune */
		if (event.data.fd == listenfd) {

			if (event.events & EPOLLIN) {

				/* in caz afirmativ, o acceptam */
				handle_new_connection();
			}

		} else {

			/* altfel, am primit un eveniment cunoscut */

			/*
			 * determinam conexiunea asociata
			 * file descriptorului specificat de eveniment
			 */
			struct connection *conn_crt = get_conn(event.data.fd);

			if (conn_crt == NULL) {
				perror("Fatal error.");
				return -1;
			}

			/*
			 * verificam daca evenimentul specifica
			 * un file descriptor de socket
			 */
			if (event.data.fd == conn_crt->sockfd) {

				/* procedam in functie de tipul evenimentului */
				if (event.events & EPOLLIN) {

					/* citim un request */
					handle_client_request(conn_crt);
				}
				if (event.events & EPOLLOUT) {
					/* trimitem in functie de tipul fisierului cerut */
					if (conn_crt->is_dyn == 1) {

						if (conn_crt->state == STATE_DATA_RECEIVED ||
							conn_crt->state == STATE_HEADER_PARTIAL_SENT) {

							send_header(conn_crt);
						} else {
							send_file_dyn(conn_crt);
						}
					} else {
						send_message(conn_crt);
					}
				}
			} else if (event.data.fd == conn_crt->ev_fd) {

				/* evenimentul specifica un fd asociat unei operatii I/O */

				int num_events = 0;
				struct io_event events[1];

				/* preluam evenimentul */
				num_events = io_getevents(conn_crt->ctx, 0, 1, events, NULL);
				DIE(num_events < 0, "io_get_events");

				if (num_events == 0)
					return conn_crt->state;

				struct iocb *crt_iocb = events[0].obj;

				conn_crt->recv_len = events[0].res;
				free(crt_iocb);

				conn_crt->left_bytes_buffer = conn_crt->recv_len;
				conn_crt->aio_state = STATE_READY;

				/*
				 * eliminam evenimentul asociat
				 * operatiei I/O anterior notificate
				 */
				rc = w_epoll_remove_fd(epollfd, conn_crt->ev_fd);
				DIE(rc < 0, "w_epoll_remove_fd");
			}
		}
	}

	return 0;
}
