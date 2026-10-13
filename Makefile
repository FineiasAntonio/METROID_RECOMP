CC = clang
CFLAGS = -O1 -fPIE -fno-strict-aliasing -w -Irt
SRCS = $(wildcard build/lifted/*/s*.c) $(wildcard build/lifted/*/table.c)
OBJS = $(SRCS:.c=.o)

RT = $(filter-out rt/main.c rt/smoke.c,$(wildcard rt/*.c))
RTH = $(wildcard rt/*.h)

build/dread: rt/main.c $(RT) $(RTH) build/libdread.a
	$(CC) -O1 -g -fPIE -pie -Wall -Irt -Irt/sdlshim rt/main.c $(RT) build/libdread.a /usr/lib64/libSDL2-2.0.so.0 -lm -lpthread -o $@

build/smoke: rt/smoke.c $(RT) $(RTH) build/libdread.a
	$(CC) -O1 -fPIE -pie -Irt -Irt/sdlshim rt/smoke.c $(RT) build/libdread.a /usr/lib64/libSDL2-2.0.so.0 -lm -lpthread -o $@

build/libdread.a: $(OBJS)
	rm -f $@ && ar rcs $@ $^

build/lifted/%.o: build/lifted/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lift:
	python3 tools/funcmap.py && python3 tools/lift.py

test: build/smoke
	./build/smoke

.PHONY: lift test
