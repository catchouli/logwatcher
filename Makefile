SOURCEFILES=main.c stringstream.c
EXECUTABLE=logwatcher

INCLUDES=-I$(INCDIR)

SRCDIR=src
OBJDIR=obj
BINDIR=bin
INCDIR=include

CFLAGS=-c -Wall -g
LDFLAGS=-L/usr/local/lib/ -lmicrohttpd -lsqlite3
SOURCES=$(patsubst %.c, $(SRCDIR)/%.c, $(SOURCEFILES))
OBJECTS=$(patsubst %.c, $(OBJDIR)/%.o, $(SOURCEFILES))
OUTPUT=$(BINDIR)/$(EXECUTABLE)

all: $(SOURCES) $(OBJECTS) $(OUTPUT)
	
	
$(OUTPUT): $(OBJECTS)
	@mkdir -p bin
	$(CC) $(LDFLAGS) $(OBJECTS) $(LIBRARIES) -o $@

%.o:
	@mkdir -p obj
	$(CC) $(patsubst $(OBJDIR)/%.o, $(SRCDIR)/%.c, $@) $(INCLUDES) $(CFLAGS) $< -o $@

clean:
	rm -rf $(OBJDIR)
	rm -rf $(BINDIR)
