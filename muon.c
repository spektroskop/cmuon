#include "muon.h"
#include "node.h"

struct geometry {
    unsigned x, y, w, h;
};

#define monitor_node(ptr) node_entry(ptr, struct monitor, node)
#define window_node(ptr) node_entry(ptr, struct window, node)
#define first_window(head) node_first_entry(head, struct window, node)

struct monitor {
    unsigned            id;
    struct geometry     geometry;
    struct geometry     base_geometry;
    struct geometry     padding;
    struct node         windows;
    unsigned            window_count;
    unsigned            root_count;
    unsigned            floating_count;
    float               root_size;
    unsigned            mirror;
    unsigned            layout;
    unsigned            border_width;
    unsigned            window_gap;
    struct window       *curwin;
    struct window       *fullscreen;

    struct node         node;
};

struct window {
    char                name[MAXLEN];
    xcb_window_t        id;
    struct window      *transient;
    struct geometry     geometry;
    struct monitor      *monitor;
    unsigned            floating;
    unsigned            px, py;
    unsigned            fullscreen;

    struct node         node;
};

enum pointer_action {
    ACTION_NONE,
    ACTION_RESIZE,
    ACTION_MOVE
};

struct pointer {
    struct window       *window;
    enum pointer_action action;
    unsigned            x, y;
    struct geometry     geometry;
};

struct rule {
    char                name[MAXLEN];
    unsigned            floating;
    unsigned            fullscreen;

    struct node         node;
};

xcb_connection_t        *connection;
xcb_ewmh_connection_t   *ewmh;
xcb_screen_t            *screen;
xcb_window_t            root;
int                     default_screen;
unsigned                w, h;
unsigned                running = true;
xcb_atom_t              wm_delete_window_atom;
xcb_atom_t              wm_protocols_atom;
unsigned                batch = false;

LIST(monitors);
LIST(rules);

struct monitor          *curmon = NULL;
struct pointer          *pointer = NULL;

unsigned                active_border_color = 0;
unsigned                inactive_border_color = 0;

xcb_get_geometry_reply_t *
get_geometry(xcb_window_t id) {
    return xcb_get_geometry_reply(connection,
        xcb_get_geometry(connection, id), NULL);
}

xcb_get_window_attributes_reply_t *
get_attributes(xcb_window_t id) {
    return xcb_get_window_attributes_reply(connection,
        xcb_get_window_attributes(connection, id), NULL);
}

unsigned
parse_color(char *color) {
    xcb_colormap_t map = screen->default_colormap;
    unsigned r, g, b, p = 0;

    if(sscanf(color + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        r *= 0x101; g *= 0x101; b *= 0x101;
        xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(connection,
            xcb_alloc_color(connection, map, r, g, b), NULL);

        if(reply) {
            p = reply->pixel;
            free(reply);
        }
    }

    return p;
}

xcb_atom_t
intern_atom(const char *name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
        xcb_intern_atom_unchecked(connection, 0,
            strlen(name), name), NULL);
    xcb_atom_t atom = reply->atom;
    free(reply);

    return atom;
}

bool
xinerama_is_active(void) {
    bool xa = false;

    if(xcb_get_extension_data(connection, &xcb_xinerama_id)->present) {
        xcb_xinerama_is_active_reply_t* reply =
            xcb_xinerama_is_active_reply(connection,
                xcb_xinerama_is_active(connection), NULL);

        if(reply) {
            xa = reply->state;
            free(reply);
        }
    }

    return xa;
}

void
flush(void) {
    if(batch) return;

    xcb_flush(connection);

    debug("flush");
}

unsigned
contains(const struct geometry *geometry, unsigned x, unsigned y) {
    return
        geometry->x <= x && x < (geometry->x + geometry->w) &&
        geometry->y <= y && y < (geometry->y + geometry->h);
}

struct monitor *
get_monitor_from_point(unsigned x, unsigned y) {
    struct monitor *monitor;

    each_node_entry(monitor, &monitors, node) {
        if(contains(&monitor->geometry, x, y)) {
            return monitor;
        }
    }

    return NULL;

}

struct monitor *
get_monitor_from_id(unsigned id) {
    struct monitor *monitor;

    each_node_entry(monitor, &monitors, node) {
        if(monitor->id == id) {
            return monitor;
        }
    }

    return NULL;
}

void
float_window(struct window *);

void
reset_layout(struct monitor *monitor) {
    monitor->root_count = ROOT_COUNT;
    monitor->root_size = ROOT_SIZE;
    monitor->mirror = MIRROR;
    monitor->layout = VERTICAL;
    monitor->window_gap = WINDOW_GAP;
    monitor->border_width = BORDER_WIDTH;
    monitor->fullscreen = NULL;
    monitor->floating_count = 0;

    struct window *window;
    each_node_entry(window, &monitor->windows, node) {
        window->floating = false;
    }

    struct rule *rule;
    each_node_entry(rule, &rules, node) {
        each_node_entry(window, &monitor->windows, node) {
            if(streq(rule->name, window->name)) {
                if(rule->floating) float_window(window);
            }
        }
    }
}

void
resize_monitor(struct monitor *monitor) {
    monitor->geometry = monitor->base_geometry;

    monitor->geometry.x += monitor->padding.x;
    monitor->geometry.w -= monitor->padding.w;

    monitor->geometry.y += monitor->padding.y;
    monitor->geometry.h -= monitor->padding.h;
}

void
add_monitor(unsigned x, unsigned y, unsigned w, unsigned h) {
    struct monitor *monitor = malloc(sizeof(*monitor));
    static unsigned id = 0;

    monitor->id = ++id;
    monitor->curwin = NULL;
    monitor->window_count = 0;
    monitor->base_geometry = (struct geometry) { x, y, w, h };
    monitor->padding = (struct geometry) { 0, 0, 0, 0 };
    monitor->fullscreen = NULL;

    resize_monitor(monitor);

    node_init(&monitor->windows);
    node_append(&monitor->node, &monitors);

    if(!curmon) {
        curmon = monitor;
    }

    reset_layout(monitor);

    p("add monitor -> %d, %dx%d+%d+%d", monitor->id, w, h, x, y);
}

void
move_resize(struct window *window, struct geometry  *geom) {
    unsigned mask =
        XCB_CONFIG_WINDOW_X|
        XCB_CONFIG_WINDOW_Y|
        XCB_CONFIG_WINDOW_WIDTH|
        XCB_CONFIG_WINDOW_HEIGHT;
    unsigned v[] = { geom->x, geom->y, geom->w, geom->h };

    xcb_configure_window(connection, window->id, mask, v);
}

void
move(struct window *window, unsigned x, unsigned y) {
    unsigned mask = XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y;
    unsigned v[] = { x, y };

    xcb_configure_window(connection, window->id, mask, v);
}

void
resize(struct window *window, unsigned w, unsigned h) {
    unsigned mask = XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT;
    unsigned v[] = { w, h };

    xcb_configure_window(connection, window->id, mask, v);
}

void lower(struct window *window) {
    unsigned v[] = { XCB_STACK_MODE_BELOW };

    xcb_configure_window(connection, window->id,
        XCB_CONFIG_WINDOW_STACK_MODE, v);
}

void raise(struct window *window) {
    unsigned v[] = { XCB_STACK_MODE_ABOVE };

    xcb_configure_window(connection, window->id,
        XCB_CONFIG_WINDOW_STACK_MODE, v);
}

void
set_border_width(struct window *window, unsigned width) {
    unsigned v[1] = { width };

    xcb_configure_window(connection, window->id,
        XCB_CONFIG_WINDOW_BORDER_WIDTH, v);
}

void
update_border_color(const struct window *window) {
    unsigned border_color = inactive_border_color;

    if(window == window->monitor->curwin) {
        border_color = active_border_color;
    }

    xcb_change_window_attributes(connection, window->id,
        XCB_CW_BORDER_PIXEL, &border_color);
}

void
set_border_color(struct window *window, unsigned color) {
    xcb_change_window_attributes(connection, window->id,
        XCB_CW_BORDER_PIXEL, &color);
}

void
focus(struct window *window) {
    if(curmon->curwin == window) return;

    if(curmon->curwin) {
        set_border_color(curmon->curwin, inactive_border_color);
    }

    if(window) {
        xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
            window->id, XCB_CURRENT_TIME);
        set_border_color(window, active_border_color);
        xcb_ewmh_set_active_window(ewmh, default_screen, window->id);
        curmon->curwin = window;

        p("focus window 0x%08x, monitor %d", window->id,
            window->monitor->id);
    } else {
        xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
            root, XCB_CURRENT_TIME);
        curmon->curwin = NULL;

        p("focus root");
    }
}

void
delete_window(struct window *window) {
    xcb_client_message_event_t event = {
        .response_type  = XCB_CLIENT_MESSAGE,
        .window         = window->id,
        .format         = 32,
        .sequence       = 0,
        .type           = wm_protocols_atom,
        .data.data32[0] = wm_delete_window_atom,
        .data.data32[1] = XCB_CURRENT_TIME,
    };

    xcb_send_event(connection, 0, window->id,
        XCB_EVENT_MASK_NO_EVENT, (char*)&event);
}

struct window *
find_window(xcb_window_t id) {
    struct monitor *monitor;
    struct window *window;

    each_node_entry(monitor, &monitors, node) {
        each_node_entry(window, &monitor->windows, node) {
            if(window->id == id) {
                return window;
            }
        }
    }

    return NULL;
}

struct window *
query_pointer(unsigned *root_x, unsigned *root_y) {
    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, root), NULL);

    struct window *window = find_window(reply->child);

    if(root_x) *root_x = reply->root_x;
    if(root_y) *root_y = reply->root_y;

    return window;
}

struct window *
first_tile(struct node *head) {
    struct window *window;

    each_node_entry(window, head, node) {
        if(!window->floating) {
            return window;
        }
    }

    return NULL;
}

struct window *
next_tile(struct window *window) {
    if(window->monitor->window_count > window->monitor->floating_count) {
        struct window *next = window_node(node_next(&window->node,
            &window->monitor->windows));

        if(next && next->floating) {
            return next_tile(next);
        }

        return next;
    }

    return NULL;
}

struct window *
process(struct window *window, const char *p, unsigned x, unsigned y, unsigned w, unsigned h) {
    p(" [%s] 0x%08x %dx%d+%d+%d", p, window->id, w, h, x, y);
    set_border_width(window, window->monitor->border_width);
    window->geometry = (struct geometry) { x, y, w, h };
    move_resize(window, &window->geometry);

    return next_tile(window);
}

void
arrange(struct monitor *monitor) {
    if(batch) return;

    unsigned wc = monitor->window_count - monitor->floating_count;

    if(!wc) return;

    p("arrange monitor %d", monitor->id);

    if(monitor->fullscreen) {
        p(" *fullscreen");
        return;
    }

    struct window *window = first_tile(&monitor->windows);

    if(wc == 1) {
        window->geometry = monitor->geometry;
        move_resize(window, &window->geometry);
        set_border_width(window, 0);
        return;
    }

    unsigned rc = monitor->root_count;
    if(rc > wc) rc = wc;
    unsigned subs = wc - rc;
    unsigned x, y, w, h, i, s, r;
    unsigned b = monitor->border_width * 2;

    switch(monitor->layout) {
        case VERTICAL: {
            r = monitor->geometry.w * monitor->root_size;

            s = monitor->geometry.h / rc;
            x = monitor->mirror ? monitor->geometry.x + (subs ? monitor->geometry.w - r : 0) : monitor->geometry.x;
            y = monitor->geometry.y;
            w = subs ? r - b : monitor->geometry.w - b;
            h = s - b;

            for(i = 1; i <= rc; i++) {
                if(i == rc) h = monitor->geometry.h - y - b;
                window = process(window, "+", x, y, w, h);
                y += s + monitor->window_gap;
            }

            if(!subs) return;

            s = monitor->geometry.h / subs;
            x = monitor->mirror ? monitor->geometry.x : monitor->geometry.x + r + monitor->window_gap;
            y = monitor->geometry.y;
            w = monitor->geometry.w - b - r - monitor->window_gap;
            h = s - b;

            for(i = 1; i <= subs; i++) {
                if(i == subs) h = monitor->geometry.h - y - b;
                window = process(window, "-", x, y, w, h);
                y += s + monitor->window_gap;
            }

            break;
        }

        case HORIZONTAL: {
            r = monitor->geometry.h * monitor->root_size;

            s = monitor->geometry.w / rc;
            x = monitor->geometry.x;
            y = monitor->mirror ? monitor->geometry.y + (subs ? monitor->geometry.h - r : 0) : monitor->geometry.y;
            w = s - b;
            h = subs ? r - b : monitor->geometry.h - b;

            for(i = 1; i <= rc; i++) {
                if(i == rc) w = monitor->geometry.w - x - b;
                window = process(window,"+", x, y, w, h);
                x += s + monitor->window_gap;
            }

            if(!subs) return;

            s = monitor->geometry.w / subs;
            x = monitor->geometry.x;
            y = monitor->mirror ? monitor->geometry.y : monitor->geometry.y + r + monitor->window_gap;
            w = s - b;
            h = monitor->geometry.h - b - r - monitor->window_gap;

            for(i = 1; i <= subs; i++) {
                if(i == subs) w = monitor->geometry.w - x - b;
                window = process(window, "-", x, y, w, h);
                x += s + monitor->window_gap;
            }

            break;
        }
    }
}

void
print_window(struct window *window) {
    xcb_get_geometry_reply_t *geom = get_geometry(window->id);

    p(" id:              0x%08x", window->id);
    p(" stored-geometry: %dx%d+%d+%d", window->geometry.w, window->geometry.h, window->geometry.x, window->geometry.y);
    p(" real-geometry:   %dx%d+%d+%d", geom->width, geom->height, geom->x, geom->y);
    p(" class:           %s", window->name);
    p(" monitor:         %u", window->monitor->id);
    p(" fullscreen:      %s", window->fullscreen ? "true" : "false");
    p(" floating:        %s", window->floating ? "true" : "false");

    if(window->transient) {
        p(" transient for:   0x%08x -> %s", window->transient->id, window->transient->name);
    } else {
        p(" transient for:   n/a");
    }

    free(geom);
}

void
store_geometry(struct window *window) {
    xcb_get_geometry_reply_t *geom = get_geometry(window->id);

    window->geometry = (struct geometry) {
        geom->x, geom->y, geom->width, geom->height
    };

    free(geom);
}

void
float_window(struct window *window) {
    if(window->floating) return;

    p("floating window 0x%08x -> `%s'", window->id, window->name);

    raise(window);

    window->floating = true;
    window->monitor->floating_count += 1;
    store_geometry(window);
}

void
toggle_floating(struct window *window) {
    if(window->floating) {
        window->floating = false;
        window->monitor->floating_count -= 1;
        lower(window);
    } else  {
        window->floating = true;
        window->monitor->floating_count += 1;
        raise(window);
    }
}

void
toggle_fullscreen(struct window *window) {
    struct monitor *monitor;

    // OK?
    each_node_entry(monitor, &monitors, node) {
        if(monitor != window->monitor && monitor->fullscreen) {
            toggle_fullscreen(monitor->fullscreen);
        }
    }

    monitor = window->monitor;

    if(window->fullscreen) {
        p("unset fullscreen");
        window->fullscreen = false;
        window->monitor->fullscreen = NULL;
        xcb_atom_t atoms[] = { XCB_NONE };
        xcb_ewmh_set_wm_state(ewmh, window->id, LENGTH(atoms), atoms);
        if(!window->floating) {
            lower(window);
            arrange(monitor);
        } else {
            move_resize(window, &window->geometry);
        }
    } else {
        p("set fullscreen");
        window->fullscreen = true;
        monitor->fullscreen = window;
        xcb_atom_t atoms[] = { ewmh->_NET_WM_STATE_FULLSCREEN };
        xcb_ewmh_set_wm_state(ewmh, window->id, LENGTH(atoms), atoms);
        set_border_width(window, 0);
        move_resize(window, &monitor->geometry);
        raise(window);
    }
}

void
update_client_list(void) {
    struct monitor *monitor;
    struct window *window;
    unsigned n = 0;

    each_node_entry(monitor, &monitors, node)
        each_node_entry(window, &monitor->windows, node)
            n++;

    if(!n) {
        xcb_ewmh_set_client_list(ewmh, default_screen, 0, NULL);
    }

    xcb_window_t windows[n];
    unsigned i = 0;

    each_node_entry(monitor, &monitors, node)
        each_node_entry(window, &monitor->windows, node)
            windows[i++] = window->id;

    xcb_ewmh_set_client_list(ewmh, default_screen, n, windows);
}

struct window *
add_window(struct monitor *monitor, xcb_window_t id) {
    struct window *window = malloc(sizeof(*window));

    window->monitor = monitor;
    window->id = id;
    window->floating = false;
    window->fullscreen = false;

    monitor->window_count += 1;

    xcb_icccm_get_wm_class_reply_t class;

    if(xcb_icccm_get_wm_class_reply(connection, xcb_icccm_get_wm_class(connection, id), &class, NULL)) {
        strncpy(window->name, class.class_name, sizeof(window->name));
        xcb_icccm_get_wm_class_reply_wipe(&class);
    }

    p("add window 0x%08x -> `%s', monitor %d", id, window->name, monitor->id);

    xcb_window_t transient = XCB_NONE;

    xcb_icccm_get_wm_transient_for_reply(connection, xcb_icccm_get_wm_transient_for_unchecked(connection, id), &transient, NULL);

    if((window->transient = find_window(transient))) {
        p("window is transient for 0x%08x -> `%s'", window->transient->id, window->transient->name);
        float_window(window);

        unsigned x = window->transient->geometry.x + (window->transient->geometry.w / 2) - (window->geometry.w / 2);
        unsigned y = window->transient->geometry.y + (window->transient->geometry.h / 2) - (window->geometry.h / 2);

        move(window, x, y); // FIXME
    }

    xcb_ewmh_get_atoms_reply_t atoms;

    if(xcb_ewmh_get_wm_window_type_reply(ewmh, xcb_ewmh_get_wm_window_type(ewmh, window->id), &atoms, NULL)) {
        for(unsigned i = 0; i < atoms.atoms_len; i++) {
            xcb_atom_t atom = atoms.atoms[i];
            if(atom == ewmh->_NET_WM_WINDOW_TYPE_DIALOG) {
                p("window-type dialog");
                float_window(window);
                break;
            }
        }

        xcb_ewmh_get_atoms_reply_wipe(&atoms);
    }

    struct rule *rule;

    each_node_entry(rule, &rules, node) {
        if(streq(rule->name, window->name)) {
            if(rule->floating) {
                float_window(window);
            }

            if(rule->fullscreen) {
                toggle_fullscreen(window);
            }
        }
    }

    set_border_width(window, monitor->border_width);
    set_border_color(window, inactive_border_color);

    monitor->curwin ? node_insert(&window->node, &monitor->curwin->node)
                    : node_insert(&window->node, &monitor->windows);

    update_client_list(); // FIXME

    return window;
}

unsigned
remove_window(struct window *window) {
    struct monitor *monitor = window->monitor;

    p("remove window 0x%08x -> `%s', monitor %d", window->id, window->name, monitor->id);

    monitor->window_count -= 1;

    node_remove(&window->node);

    if(window->fullscreen) {
        monitor->fullscreen = NULL;
    }

    if(curmon->root_count > curmon->window_count) {
        curmon->root_count = curmon->window_count;
    }

    if(monitor->window_count > 0) {
        focus(window_node(node_prev(&monitor->curwin->node, &monitor->windows)));
    } else {
        curmon->curwin = NULL; // FIXME
        focus(NULL);
    }

    if(window->floating) {
        monitor->floating_count -= 1;
    } else {
        arrange(monitor);
    }

    free(window);

    return true;
}

void
substructure(void) {
    unsigned values[] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY };
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(connection, root, XCB_CW_EVENT_MASK, values);

    if(xcb_request_check(connection, cookie)) {
        d("error: could not set substructure_redirect");
    }
}

void
reparent(void) {
    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(connection, xcb_query_tree(connection, root), NULL);
    xcb_window_t *c = xcb_query_tree_children(reply);
    unsigned n = xcb_query_tree_children_length(reply);
    struct window *window = NULL;

    for(unsigned i = 0; i < n; i++) {
        xcb_get_window_attributes_reply_t *attr = get_attributes(c[i]);
        unsigned override_redirect = attr->override_redirect;
        unsigned viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;

        free(attr);

        if(override_redirect) {
            p("ignoring window 0x%08x -> override_redirect", c[i]);
            continue;
        }

        if(!viewable) {
            p("ignoring window 0x%08x -> not viewable", c[i]);
            continue;
        }

        xcb_get_geometry_reply_t *geom = get_geometry(c[i]);
        struct monitor *monitor = get_monitor_from_point(geom->x + (geom->width / 2), geom->y + (geom->height / 2));
        free(geom);
        window = add_window(monitor, c[i]);
    }

    if(window) {
        arrange(window->monitor);
        focus(window);
    }

    free(reply);
}

void
monitor_setup(void) {
    if(xinerama_is_active()) {
        xcb_xinerama_query_screens_reply_t *reply = xcb_xinerama_query_screens_reply(connection, xcb_xinerama_query_screens(connection), NULL);
        xcb_xinerama_screen_info_t *screens = xcb_xinerama_query_screens_screen_info(reply);
        unsigned n = xcb_xinerama_query_screens_screen_info_length(reply);

        for(unsigned i = 0; i < n; i++) {
            xcb_xinerama_screen_info_t screen = screens[i];
            add_monitor(screen.x_org, screen.y_org, screen.width, screen.height);
        }

        free(reply);
    } else {
        add_monitor(0, 0, w, h);
    }
}

void
ewmh_setup(void) {
    ewmh = (xcb_ewmh_connection_t *) malloc(sizeof(*ewmh));
    xcb_intern_atom_cookie_t *cookies;
    cookies = xcb_ewmh_init_atoms(connection, ewmh);
    xcb_ewmh_init_atoms_replies(ewmh, cookies, NULL);

    xcb_atom_t atoms[] = {
        ewmh->_NET_SUPPORTED,
        ewmh->_NET_CLIENT_LIST,
        ewmh->_NET_NUMBER_OF_DESKTOPS,
        ewmh->_NET_CURRENT_DESKTOP,
        ewmh->_NET_ACTIVE_WINDOW,
//      ewmh->_NET_WM_DESKTOP,
        ewmh->_NET_WM_WINDOW_TYPE,
        ewmh->_NET_WM_WINDOW_TYPE_DIALOG,
        ewmh->_NET_WM_STATE,
        ewmh->_NET_WM_STATE_FULLSCREEN,
    };

    xcb_ewmh_set_supported(ewmh, default_screen, LENGTH(atoms), atoms);
    xcb_ewmh_set_number_of_desktops(ewmh, default_screen, 1);
    xcb_ewmh_set_current_desktop(ewmh, default_screen, 0);
}

unsigned
parse_boolean(const char *param, unsigned *data) {
    if(streq(param, "toggle")) {
        *data ^= 1;
    } else if(streq(param, "false") || streq(param, "off")) {
        *data = false;
    } else if(streq(param, "true") || streq(param, "on")) {
        *data = true;
    } else {
        return false;
    }

    return true;
}

unsigned
make_root() {
    if(curmon->window_count < 2) return false;
    node_make_head(&curmon->curwin->node, &curmon->windows);
    arrange(curmon);
    return true;
}

unsigned
set_root_size(const char *param) {
    if(curmon->window_count < 2) return false;

    float cur = curmon->root_size;
    float size;

    if(!sscanf(param, "%f", &size)) return false;

    if(param[0] == '+' || param[0] == '-') {
        curmon->root_size += size;
    } else {
        curmon->root_size  = size;
    }

    if(curmon->root_size > ROOT_MAX) curmon->root_size = ROOT_MAX;
    if(curmon->root_size < ROOT_MIN) curmon->root_size = ROOT_MIN;
    if(curmon->root_size == cur) return false;

    arrange(curmon);

    return true;
}

unsigned
set_root_count(const char *param) {
    if(curmon->window_count < 2) return false;

    unsigned cur = curmon->root_count;
    int count;

    if(!sscanf(param, "%u", &count)) return false;

    if(param[0] == '+' || param[0] == '-') {
        curmon->root_count += count;
    } else {
        curmon->root_count  = count;
    }

    if(curmon->root_count > curmon->window_count) curmon->root_count = curmon->window_count;
    if(curmon->root_count < 1) curmon->root_count = 1;
    if(curmon->root_count == cur) return false;

    arrange(curmon);

    return true;
}

unsigned
shift_window(const char *param) {
    if(curmon->window_count < 2) return false;

    int count;

    if(!sscanf(param, "%d", &count)) return false;

    for(unsigned i = 0; i < abs(count); i++) {
        count < 0 ? node_unshift(&curmon->curwin->node, &curmon->windows)
                  : node_shift(&curmon->curwin->node, &curmon->windows);
    }

    arrange(curmon);

    return true;
}

unsigned
select_window(const char *param) {
    int id;

    if(param[0] == '+' || param[0] == '-') {
        if(!sscanf(param, "%d", &id)) return false;

        for(unsigned i = 0; i < abs(id); ++i) {
            id < 0 ? focus(window_node(node_prev(&curmon->curwin->node, &curmon->windows)))
                   : focus(window_node(node_next(&curmon->curwin->node, &curmon->windows)));
        }

        return true;
    } else {
        if(!sscanf(param, "%x", &id)) return false;

        struct monitor *monitor;
        struct window *window;

        if(id == curmon->curwin->id) return false;

        each_node_entry(monitor, &monitors, node) {
            each_node_entry(window, &monitor->windows, node) {
                if(window->id == id) {
                    set_border_color(curmon->curwin, inactive_border_color);
                    focus(window);
                    return true;
                }
            }
        }

        return false;
    }
}

void
get_parameter(const char *name, char *response) {
    if(streq(name, "root-size")) {
        snprintf(response, BUFSIZ, "%f\n", curmon->root_size);
    } else if(streq(name, "root-count")) {
        snprintf(response, BUFSIZ, "%u\n", curmon->root_count);
    } else if(streq(name, "window-gap")) {
        snprintf(response, BUFSIZ, "%u\n", curmon->window_gap);
    } else if(streq(name, "border-width")) {
        snprintf(response, BUFSIZ, "%u\n", curmon->border_width);
    } else if(streq(name, "fullscreen")) {
        snprintf(response, BUFSIZ, "%s\n", curmon->fullscreen ? "true" : "false");
    } else if(streq(name, "mirror")) {
        snprintf(response, BUFSIZ, "%s\n", curmon->mirror ? "true" : "false");
    }
}

struct rule *
make_rule(const char *name) {
    struct rule *rule = malloc(sizeof(*rule));
    strncpy(rule->name, name, sizeof(rule->name));
    rule->floating = false;
    rule->fullscreen = false;
    return rule;
}

void
process_command(char *message, char *response) {
    debug("command: %s", message);

    const char *command = strtok(message, " ");
    if(!command) return;

    if(streq(command, "quit")) {
        running = false;
    } else if(streq(command, "begin")) {
        p("command sequence begin")
        batch = true;
        return;
    } else if(streq(command, "end")) {
        p("command sequence end")
        batch = false;
        arrange(curmon);
    } else if(streq(command, "debug-window")) {
        if(curmon->window_count < 1) return;
        print_window(curmon->curwin);
        return;
    } else if(streq(command, "root-count")) {
        if(curmon->window_count < 2) return;
        const char *count = strtok(NULL, " ");
        if(!count || !set_root_count(count)) return;
    } else if(streq(command, "root-size")) {
        if(curmon->window_count < 2) return;
        const char *size = strtok(NULL, " ");
        if(!size || !set_root_size(size)) return;
    } else if(streq(command, "window-gap")) {
        const char *gap = strtok(NULL, " ");
        if(!gap) return;
        sscanf(gap, "%u", &curmon->window_gap);
        arrange(curmon);
    } else if(streq(command, "border-width")) {
        const char *size = strtok(NULL, " ");
        if(!size) return;
        sscanf(size, "%u", &curmon->border_width);

        struct window *window;
        each_node_entry(window, &curmon->windows, node) {
            set_border_width(window, curmon->border_width);
        }

        arrange(curmon);
    } else if(streq(command, "padding")) {
        const char *direction = strtok(NULL, " ");
        if(!direction) return;

        const char *padding = strtok(NULL, " ");
        if(!padding) return;

        if(streq(direction, "bottom")) {
            sscanf(padding, "%u", &curmon->padding.h);
        } else if(streq(direction, "top")) {
            sscanf(padding, "%u", &curmon->padding.y);
        } else if(streq(direction, "left")) {
            sscanf(padding, "%u", &curmon->padding.x);
        } else if(streq(direction, "right")) {
            sscanf(padding, "%u", &curmon->padding.w);
        } else {
            return;
        }

        resize_monitor(curmon);
        arrange(curmon);
    } else if(streq(command, "fullscreen")) {
        if(curmon->window_count < 1) return;

        const char *param = strtok(NULL, " ");
        if(!param) return;

        if(streq(param, "toggle")) {
            toggle_fullscreen(curmon->curwin);
        } else if(streq(param, "false") || streq(param, "off")) {
            if(curmon->fullscreen) toggle_fullscreen(curmon->fullscreen);
        } else if(streq(param, "true") || streq(param, "on")) {
            if(!curmon->fullscreen) toggle_fullscreen(curmon->fullscreen);
        } else {
            return;
        }
    } else if(streq(command, "mirror")) {
        if(curmon->fullscreen) return;

        if(curmon->window_count < 2) return;

        const char *mirror = strtok(NULL, " ");
        if(!mirror || !parse_boolean(mirror, &curmon->mirror)) return;

        arrange(curmon);
    } else if(streq(command, "get")) {
        const char *name = strtok(NULL, " ");
        if(!name) return;

        get_parameter(name, response);

        return;
    } else if(streq(command, "make-root")) {
        if(curmon->fullscreen) return;
        make_root();
    } else if(streq(command, "select-window")) {
        if(curmon->fullscreen) return;

        const char *param = strtok(NULL, " ");
        if(!param) return;

        select_window(param);
    } else if(streq(command, "shift-window")) {
        if(curmon->fullscreen) return;

        const char *param = strtok(NULL, " ");
        if(!param) return;

        shift_window(param);
    } else if(streq(command, "next-layout")) {
        if(curmon->fullscreen) return;

        if(++curmon->layout >= LAYOUT_MAX) curmon->layout = 0;

        arrange(curmon);
    } else if(streq(command, "previous-layout")) {
        if(curmon->fullscreen) return;

        if(--curmon->layout <= 0) curmon->layout = LAYOUT_MAX;

        arrange(curmon);
    } else if(streq(command, "reset-layout")) {
        reset_layout(curmon);

        arrange(curmon);
    } else if(streq(command, "rule")) {
        const char *name = strtok(NULL, " ");
        if(!name) return;

        const char *attribute = strtok(NULL, " ");
        if(!attribute) return;

        if(streq(attribute, "floating")) {
            struct rule *rule = make_rule(name);
            rule->floating = true;
            node_append(&rule->node, &rules);
            p("adding rule `floating' to `%s'", rule->name);
        } else if(streq(attribute, "fullscreen")) {
            struct rule *rule = make_rule(name);
            rule->fullscreen = true;
            node_append(&rule->node, &rules);
            p("adding rule `fullscreen' to `%s'", rule->name);
        }

        return;
    } else if(streq(command, "grab-pointer")) {
        if(curmon->fullscreen) return;

        const char *action = strtok(NULL, " ");
        if(!action) return;

        struct window *window;
        if(!(window = query_pointer(&pointer->x, &pointer->y)))
            return;

        p("grabbing pointer for -> 0x%08x, `%s'", window->id, window->name);

        if(streq(action, "move")) {
            pointer->action = ACTION_MOVE;
        } else if(streq(action, "resize")) {
            pointer->action = ACTION_RESIZE;
        } else {
            return;
        }

        if(!window->floating) {
            float_window(window);
            arrange(window->monitor);
        }

        pointer->window = window;
        pointer->geometry = window->geometry;;

        raise(window);
        focus(window);
    } else if(streq(command, "track-pointer")) {
        const char *sx = strtok(NULL, " ");
        const char *sy = strtok(NULL, " ");
        unsigned dx, dy, x, y;

        if(!sx || !sy) return;

        sscanf(sx, "%u", &x);
        sscanf(sy, "%u", &y);

        dx = x - pointer->x;
        dy = y - pointer->y;

        move(pointer->window, pointer->geometry.x + dx, pointer->geometry.y + dy);
    } else if(streq(command, "ungrab-pointer")) {
        p("ungrabbing pointer");

        xcb_get_geometry_reply_t *geom = get_geometry(pointer->window->id);

        pointer->window->geometry.x = geom->x;
        pointer->window->geometry.y = geom->y;
        pointer->window->geometry.w = geom->height;
        pointer->window->geometry.h = geom->width;

        pointer->window = NULL;
        pointer->action = ACTION_NONE;
    } else if(streq(command, "close-window")) {
        if(!curmon->curwin) return;

        delete_window(curmon->curwin);
    } else if(streq(command, "focus-window")) {
        struct window *window;

        if(!(window = query_pointer(NULL, NULL))) return;

        if(window == curmon->curwin) return;

        focus(window);
    } else if(streq(command, "toggle-floating")) {
        if(curmon->fullscreen) return;

        if(curmon->curwin) {
            toggle_floating(curmon->curwin);
            arrange(curmon);
        }
    } else {
        snprintf(response, BUFSIZ, "unknown command: %s\n", command);
        return;
    }

    flush();
}

void
process_state(struct window *window, xcb_atom_t state, unsigned action) {
    if(state == ewmh->_NET_WM_STATE_FULLSCREEN) {
        p("  FULLSCREEN");

        if(action == XCB_EWMH_WM_STATE_TOGGLE) {
            p("   TOGGLE");
            toggle_fullscreen(window);
        } else if(action == XCB_EWMH_WM_STATE_REMOVE && window->fullscreen) {
            p("   REMOVE");
            toggle_fullscreen(window);
        } else if(action == XCB_EWMH_WM_STATE_ADD && !window->fullscreen) {
            p("   ADD");
            toggle_fullscreen(window);
        }
    }

#define pstate(id) \
    if(state == ewmh->_NET_WM_STATE_##id) { \
        p("   "#id); \
    } \

    pstate(MODAL);
    pstate(STICKY);
    pstate(MAXIMIZED_VERT);
    pstate(MAXIMIZED_HORZ);
    pstate(SHADED);
    pstate(SKIP_TASKBAR);
    pstate(SKIP_PAGER);
    pstate(HIDDEN);
    pstate(ABOVE);
    pstate(BELOW);
    pstate(DEMANDS_ATTENTION);
}

void
process_event(xcb_generic_event_t *event) {
    switch (XCB_EVENT_RESPONSE_TYPE(event)) {
        case XCB_MAP_REQUEST: {
            xcb_map_request_event_t *e = (xcb_map_request_event_t *) event;

            pwinid("map-request", e->window);

            struct window *parent = find_window(e->parent);
            if(parent) p("parent 0x%08x -> `%s'", parent->id, parent->name);

            xcb_get_window_attributes_reply_t *attr = get_attributes(e->window);
            unsigned override_redirect = attr->override_redirect;
            free(attr);

            if(override_redirect) {
                p("ignoring window 0x%08x -> override_redirect", e->window);
                return;
            }

            if(find_window(e->window)) {
                p("ignoring window 0x%08x -> already managed", e->window);
                return;
            }

            struct window *window = add_window(curmon, e->window);
            xcb_map_window(connection, e->window);

            if(!window->floating) arrange(window->monitor);

            focus(window);

            break;
        }

        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t *e = (xcb_map_notify_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            pwin("map-notify", window);

            break;
        }

        case XCB_DESTROY_NOTIFY: {
            xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            pwin("destroy-notify", window);

            if(!remove_window(window)) return;

            break;
        }

        case XCB_UNMAP_NOTIFY: {
            xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            pwin("unmap-notify", window);

            if(!remove_window(window)) return;

            break;
        }

        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *e = (xcb_client_message_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            pwin("client-message", window);

            if (e->type == ewmh->_NET_WM_STATE) {
                p(" state");

                process_state(window, e->data.data32[1], e->data.data32[0]);
                process_state(window, e->data.data32[2], e->data.data32[0]);
            }

            break;
        }

        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            if(pointer->window == window) {
                debug("ignoring configure-notify for grabbed window");
                return;
            }

            p("configure-notify for 0x%08x -> `%s'", window->id, window->name);

            break;
        }

        case XCB_CONFIGURE_REQUEST: {
            xcb_configure_request_event_t *e = (xcb_configure_request_event_t *) event;

            struct window *window;

            if(!(window = find_window(e->window))) return;

            if(pointer->window == window) {
                debug("ignoring configure-request for grabbed window");
                return;
            }

            p("configure request for 0x%08x -> `%s'", window->id, window->name);

            if(window->floating) {
                unsigned i = 0, mask = 0, values[7];

                if(e->value_mask & XCB_CONFIG_WINDOW_X) {
                    mask |= XCB_CONFIG_WINDOW_X;
                    values[i++] = e->x;
                    window->geometry.x = e->x;
                }

                if(e->value_mask & XCB_CONFIG_WINDOW_Y) {
                    mask |= XCB_CONFIG_WINDOW_Y;
                    values[i++] = e->y;
                    window->geometry.y = e->y;
                }

                if(e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
                    mask |= XCB_CONFIG_WINDOW_WIDTH;
                    values[i++] = e->width;
                    window->geometry.w = e->width;
                }

                if(e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
                    mask |= XCB_CONFIG_WINDOW_HEIGHT;
                    values[i++] = e->height;
                    window->geometry.w = e->height;
                }

                xcb_configure_window(connection, e->window, mask, values);
            } else {
                xcb_configure_notify_event_t config = {
                    .response_type = XCB_CONFIGURE_NOTIFY,
                    .event = window->id,
                    .window = window->id,
                    .above_sibling = XCB_NONE,
                    .x = window->geometry.x,
                    .y = window->geometry.y,
                    .width = window->geometry.w,
                    .height = window->geometry.h,
                    .border_width = window->monitor->border_width,
                    .override_redirect = false
                };

                xcb_send_event(connection, false, window->id, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                    (const char *) &config);
            }

            break;
        }

        default: {
            p("ignored event %d, %s", event->response_type, event_to_string(event->response_type));

            return;
         }
    }

    flush();
}

void
cleanup(void) {
    struct monitor *monitor, *m;
    struct window *window, *w;

    each_node_entry_safe(monitor, m, &monitors, node) {
        each_node_entry_safe(window, w, &monitor->windows, node)
            remove_window(window);
        node_remove(&monitor->node);
        free(monitor);
    }
}

int
main(void) {
    p("x");
    connection = xcb_connect(NULL, &default_screen);
    screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    w = screen->width_in_pixels;
    h = screen->height_in_pixels;
    root = screen->root;
    inactive_border_color = parse_color(INACTIVE_COLOR);
    active_border_color = parse_color(ACTIVE_COLOR);
    wm_delete_window_atom = intern_atom("WM_DELETE_WINDOW");
    wm_protocols_atom = intern_atom("WM_PROTOCOLS");
    pointer = malloc(sizeof(*pointer));
    pointer->window = NULL;

    substructure();
    monitor_setup();
    ewmh_setup();
    reparent();

    unsigned command_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unsigned xcb_fd = xcb_get_file_descriptor(connection);
    unsigned fdn = MAX(command_fd, xcb_fd) + 1;
    fd_set fds;
    struct sockaddr_un addr;

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/muon-socket");
    unlink(addr.sun_path);
    bind(command_fd, (struct sockaddr *) &addr, sizeof(addr));
    listen(command_fd, SOMAXCONN);

    flush();

    p("run");

    while(running) {
        FD_ZERO(&fds);
        FD_SET(command_fd, &fds);
        FD_SET(xcb_fd, &fds);

        if(select(fdn, &fds, NULL, NULL, NULL)) {
            if(FD_ISSET(command_fd, &fds)) {
                unsigned n, return_fd = accept(command_fd, NULL, 0);
                char res[BUFSIZ] = { 0 }, cmd[BUFSIZ] = { 0 };

                if(return_fd > 0 && (n = recv(return_fd, cmd, sizeof(cmd), 0)) > 0) {
                    process_command(cmd, res);
                    send(return_fd, res, strlen(res), 0);
                    close(return_fd);
                }
            }

            if(FD_ISSET(xcb_fd, &fds)) {
                xcb_generic_event_t *event;

                while((event = xcb_poll_for_event(connection))) {
                    process_event(event);
                    free(event);
                }
            }

        }
    }

    cleanup();

    close(command_fd);
    xcb_ewmh_connection_wipe(ewmh);
    free(ewmh);
    xcb_flush(connection);
    xcb_disconnect(connection);

    free(pointer);

    return 0;
}
