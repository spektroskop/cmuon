WM_SRC = muon.c
CL_SRC = muoc.c

WM_OBJ = $(WM_SRC:.c=.o)
CL_OBJ = $(CL_SRC:.c=.o)

CFLAGS += -g -Os -std=c99 -Wall -I.
LIBS += -lxcb -lxcb-util -lxcb-ewmh -lxcb-xinerama -lxcb-icccm

all: muon muoc

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

muon: $(WM_OBJ)
	$(CC) -o $@ $(WM_OBJ) $(LDFLAGS) $(LIBS)

muoc: $(CL_OBJ)
	$(CC) -o $@ $(CL_OBJ) $(LDFLAGS)

clean:
	rm -f $(WM_OBJ) $(CL_OBJ) muon muoc

all: $(TARGET)
