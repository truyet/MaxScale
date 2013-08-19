include ../build_gateway.inc
include ../makefile.inc

CC = gcc
CPP = g++

SRCS = query_classifier.cc

QUERY_CLASSIFIER_PATH := $(shell pwd)

makeall: clean all

clean:
	make -C ../utils clean
	- $(DEL) query_classifier.o 
	- $(DEL) libquery_classifier.so
	- $(DEL) libquery_classifier.so.1.0.1 
	- $(DEL) skygw_utils.o
	- $(DEL) *~
	- $(DEL) depend

all: utils lib

utils:
	make -C $(ROOT_PATH)/utils clean all
	$(COPY) $(ROOT_PATH)/utils/skygw_utils.o ./

lib: libcomp liblink

libcomp: 
	$(CPP) -c $(CFLAGS) \
	-I$(MARIADB_SRC_PATH)/libmysqld/ \
	-I$(MARIADB_SRC_PATH)/include/ \
	-I$(MARIADB_SRC_PATH)/sql \
	-I$(MARIADB_SRC_PATH)/regex/ \
	-I./ \
	-fPIC ./query_classifier.cc -o query_classifier.o

liblink:
	 $(CPP) -shared \
	-L$(MARIADB_SRC_PATH)/libmysqld \
	-Wl,-soname,libquery_classifier.so \
	-Wl,-rpath,$(DEST)/lib \
	-Wl,-rpath,$(MARIADB_SRC_PATH)/libmysqld \
	-o libquery_classifier.so.1.0.1 ./query_classifier.o \
	$(LDLIBS) $(LDMYSQL) $(CPP_LDLIBS)
	$(DEL) ./libquery_classifier.so
	$(LINK) ./libquery_classifier.so.1.0.1 ./libquery_classifier.so


install: liblink
	install ./libquery_classifier.so.1.0.1 ./libquery_classifier.so $(DEST)/lib

depend:
	@rm -f depend
	$(CPP) -M $(CFLAGS) \
	-I$(MARIADB_SRC_PATH)/libmysqld/ \
	-I$(MARIADB_SRC_PATH)/include/ \
	-I$(MARIADB_SRC_PATH)/sql \
	-I$(MARIADB_SRC_PATH)/regex/ \
	-I./ \
	$(SRCS) > depend

include depend