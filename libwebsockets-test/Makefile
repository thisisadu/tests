.PHONY:clean

all:example-client example-server

example-client:example-client.c
	gcc -I./include  example-client.c -o example-client -L. -lpthread -lwebsockets -lssl -lcrypto -lz

example-server:example-server.c
	gcc -I./include example-server.c -o example-server -L. -lwebsockets -lssl -lcrypto -lz

clean:
	-rm example-client example-server
