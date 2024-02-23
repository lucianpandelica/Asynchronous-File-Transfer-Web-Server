CC = gcc
CFLAGS = -fPIC -Wall -g
CEXTLIB = -laio

.PHONY: build
build: aws

aws: sock_util.o aws.o http_parser.o
	$(CC) $(CFLAGS) -o aws $^ $(CEXTLIB)

aws.o: aws.c aws.h w_epoll.h
	$(CC) $(CFLAGS) -o $@ -c $<

sock_util.o: sock_util.c sock_util.h w_epoll.h
	$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	-rm -f aws.o sock_util.o http_parser.o aws
