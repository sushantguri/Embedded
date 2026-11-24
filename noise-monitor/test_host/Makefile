CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DHOST_TEST -I../main/include
LDFLAGS := -lm

SRCS := \
	test_noise_monitor.c \
	../main/processing.c \
	../main/display.c \
	../main/uart.c \
	../main/web_dashboard.c

TARGET := test_noise_monitor

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
