OBJ:=cmd_parser.o
CFLAGS:=-fPIC -c -Wall -O -g
TARGET=libcmd_parser.so

all:$(TARGET)

$(TARGET):$(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LIB) 

$(OBJ):%.o:%.c
	$(CC) $(CFLAGS) $< -o $@

.PHONY:clean
clean:
	rm -rf $(OBJ) $(TARGET)

