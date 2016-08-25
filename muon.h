#ifndef MUON_H
#define MUON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xinerama.h>

#define DEBUG false

#define streq(a, b)             (!strcmp((a), (b)))
#define __p(m, f, ...)          do { fprintf(stdout, m "\n", ##__VA_ARGS__); f; } while(0);
#define p(m, ...)               __p(m, fflush(stdout), ##__VA_ARGS__);
#define d(m, ...)               __p(m, exit(1), ##__VA_ARGS__);
#define debug(m, ...)           if(DEBUG) p(m, ##__VA_ARGS__);
#define pwinid(prefix, id)      printf(prefix " for 0x%08x\n", id)
#define pwin(prefix, object)    printf(prefix " for 0x%08x -> `%s'\n", object->id, object->name)

#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define LENGTH(x)               (sizeof(x) / sizeof(*x))

#define MAXLEN                  256
#define ROOT_MAX                0.9
#define ROOT_MIN                0.1
#define HORIZONTAL              0
#define VERTICAL                1
#define LAYOUT_MAX              2

#define ROOT_COUNT              1
#define ROOT_SIZE               0.65
#define INACTIVE_COLOR          "#3F3E3B"
#define ACTIVE_COLOR            "#11809E"
#define MIRROR                  false
#define WINDOW_GAP              1
#define BORDER_WIDTH            5

const char *event_to_string(unsigned id) {
    switch(id) {
        case 2:   return "keypress";
        case 3:   return "keyrelease";
        case 4:   return "buttonpress";
        case 5:   return "buttonrelease";
        case 6:   return "motionnotify";
        case 7:   return "enternotify";
        case 8:   return "leavenotify";
        case 9:   return "focusin";
        case 10:  return "focusout";
        case 11:  return "keymapnotify";
        case 12:  return "expose";
        case 13:  return "graphicsexpose";
        case 14:  return "noexpose";
        case 15:  return "visibilitynotify";
        case 16:  return "createnotify";
        case 17:  return "destroynotify";
        case 18:  return "unmapnotify";
        case 19:  return "mapnotify";
        case 20:  return "maprequest";
        case 21:  return "reparentnotify";
        case 22:  return "configurenotify";
        case 23:  return "configurerequest";
        case 24:  return "gravitynotify";
        case 25:  return "resizerequest";
        case 26:  return "circulatenotify";
        case 27:  return "circulaterequest";
        case 28:  return "propertynotify";
        case 29:  return "selectionclear";
        case 30:  return "selectionrequest";
        case 31:  return "selectionnotify";
        case 32:  return "colormapnotify";
        case 33:  return "clientmessage";
        case 34:  return "mappingnotify";
        default:  return "--";
    }
    return NULL;
}

#endif
