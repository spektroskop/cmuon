#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define LIST_INIT(name) { &(name), &(name) }
#define LIST(name) struct node name = LIST_INIT(name)

#define node_entry(ptr, type, member) ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))
#define node_first_entry(ptr, type, member) node_entry((ptr)->next, type, member)
#define each_node(pos, head) for(pos = (head)->next; pos != (head); pos = pos->next)
#define each_node_entry(pos, head, member) \
    for(pos = node_entry((head)->next, __typeof__(*pos), member); \
       &pos->member != (head); \
        pos = node_entry(pos->member.next, __typeof__(*pos), member))
#define each_node_entry_from(pos, node, member) \
    for(; &pos->member != (node); \
           pos = node_entry(pos->member.next, __typeof__(*pos), member))

#define each_node_entry_safe(pos, n, node, member) \
    for(pos = node_entry((node)->next, __typeof__(*pos), member), \
          n = node_entry(pos->member.next, __typeof__(*pos), member); \
       &pos->member != (node); \
        pos = n, \
          n = node_entry(n->member.next, __typeof__(*n), member))

struct node {
    struct node *next, *prev;
};

static inline void node_init(struct node *node) {
    node->next = node->prev = node;
}

static inline void __node_add(struct node *new, struct node *prev, struct node *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void __node_remove(struct node *prev, struct node *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void node_insert(struct node *new, struct node *head) {
    __node_add(new, head, head->next);
}

static inline void node_append(struct node *new, struct node *head) {
    __node_add(new, head->prev, head);
}

static inline void node_remove(struct node *node) {
    __node_remove(node->prev, node->next);
}

static inline void node_move_insert(struct node *node, struct node *head) {
    node_remove(node);
    node_insert(node, head);
}

static inline void node_move_append(struct node *node, struct node *head) {
    node_remove(node);
    node_append(node, head);
}

static inline bool node_is_empty(const struct node *node) {
    return node->next == node;
}

static inline bool node_is_singular(const struct node *node) {
    return !node_is_empty(node) && (node->next == node->prev);
}

static inline bool node_is_last(const struct node *node, const struct node *head) {
    return node->next == head;
}

static inline struct node *node_next(struct node *node, const struct node *head) {
    if(node_is_empty(head) || node_is_singular(head)) return node;
    if(node->next == head) return head->next;
    return node->next;
}

static inline struct node *node_prev(struct node *node, const struct node *head) {
    if(node_is_empty(head) || node_is_singular(head)) return node;
    if(node->prev == head) return head->prev;
    return node->prev;
}

static inline void node_shift(struct node *node, struct node *head) {
    if(node_is_empty(head) || node_is_singular(head)) return;
    if(node->next == head) node_move_insert(node, head);
    else node_move_insert(node, node->next);
}

static inline void node_unshift(struct node *node, struct node *head) {
    if(node_is_empty(head) || node_is_singular(head)) return;
    if(node->prev == head) node_move_append(node, head);
    else node_move_append(node, node->prev);
}

static inline void node_make_head(struct node *node, struct node *head) {
    if(node_is_empty(head) || node_is_singular(head)) return;
    node_move_insert(node, head);
}
