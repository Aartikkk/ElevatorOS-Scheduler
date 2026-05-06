#===========================================================================================================================================================================
# Title       : Makefile
# Description : Builds the Elevator Operating System Scheduler for the CS4352 Final Project.
# Author      : Aarti Krishan Khatri (R11860380)
# Date        : 05/05/2026
# Version     : 1.0
# Usage       : make / make clean
# Notes       : Produces executable named scheduler_os. Requires pthreads.
# C Version   : C11
#===========================================================================================================================================================================

CC = gcc
CFLAGS = -Wall -Wextra -pthread -std=c11
TARGET = scheduler_os
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) -lm

clean:
	rm -f $(TARGET)