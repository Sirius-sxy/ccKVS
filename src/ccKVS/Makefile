LD      := gcc-6
LDFLAGS := -O3 -std=gnu11 -flto -lgsl -lgslcblas -libverbs -lrt -lpthread -lmemcached -lnuma -lrdmacm
CC  = gcc-6
CFLAGS   = -O3 -std=gnu11  -I../../include/mica -I../../include/libhrd -I../../include/ccKVS -I../../include/optik
APPS    := ccKVS-sc ccKVS-lin ccKVS-server-sc ccKVS-server-lin ccKVS-client-sc ccKVS-client-lin clean-o
PROF    := -g -fno-omit-frame-pointer

all: ${APPS}

# Original unified executables (deprecated, for backward compatibility)
ccKVS-sc: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main.o client-sc.o worker.o cache.o util.o stats.o
	${LD} -o $@ $^ ${LDFLAGS}

ccKVS-lin: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main.o client-lin.o worker.o cache.o util.o stats.o
	${LD}  -o $@ $^ ${LDFLAGS}

# Separated server executables (with server-side cache)
ccKVS-server-sc: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main-server.o worker.o worker-cache.o cache.o util.o stats.o
	${LD} -o $@ $^ ${LDFLAGS}

ccKVS-server-lin: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main-server.o worker.o worker-cache.o cache.o util.o stats.o
	${LD} -o $@ $^ ${LDFLAGS}

# Separated client executables
ccKVS-client-sc: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main-client.o client-sc.o cache.o util.o stats.o
	${LD} -o $@ $^ ${LDFLAGS}

ccKVS-client-lin: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o ../mica/mica.o ../mica/city.o main-client.o client-lin.o cache.o util.o stats.o
	${LD} -o $@ $^ ${LDFLAGS}

# Convenience targets
server: ccKVS-server-sc ccKVS-server-lin

client: ccKVS-client-sc ccKVS-client-lin

separated: server client

PHONY: clean server client separated
clean:
	rm -f *.o ../libhrd/*.o ../mica/*.o ${APPS}
clean-o:
	rm -f *.o ../libhrd/*.o ../mica/*.o
