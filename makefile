all: proxy

proxy: csapp.o proxy.o
	gcc -lpthread csapp.o proxy.o -o proxy

csapp.o: csapp.c
	gcc -c csapp.c -o csapp.o

proxy.o: proxy.c
	gcc -c proxy.c -o proxy.o

clean:
	rm *o proxy