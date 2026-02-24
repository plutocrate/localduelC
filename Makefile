# Makefile for DUEL - Local 2-Player Fencing Game
# Requires raylib installed

CC      = gcc
TARGET  = duel

SRCS    = main.c game.c player.c physics.c combat.c input.c audio.c
OBJS    = $(SRCS:.c=.o)

CFLAGS  = -std=c11 -Wall -Wextra -O2 -g

# ---- Linux ----
LINUX_LIBS = -lraylib -lm -lpthread -ldl

# ---- Windows (MinGW cross-compile from Linux, or native MinGW) ----
WIN_CC     = x86_64-w64-mingw32-gcc
WIN_TARGET = duel.exe
WIN_LIBS   = -lraylib -lwinmm -lgdi32 -lopengl32 -lm

.PHONY: all linux windows clean

all: linux

linux: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LINUX_LIBS)
	@echo "Built: $(TARGET)"

windows: $(SRCS)
	$(WIN_CC) $(CFLAGS) $(SRCS) -o $(WIN_TARGET) $(WIN_LIBS)
	@echo "Built: $(WIN_TARGET)"

clean:
	rm -f $(OBJS) $(TARGET) $(WIN_TARGET)

# If raylib is installed to a custom path, override RAYLIB_PATH:
# make linux CFLAGS="-std=c11 -Wall -O2 -I/opt/raylib/include" LINUX_LIBS="-L/opt/raylib/lib -lraylib -lm -lpthread -ldl"
