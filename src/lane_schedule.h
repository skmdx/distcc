#ifndef DCC_LANE_SCHEDULE_H
#define DCC_LANE_SCHEDULE_H
#include "broker_protocol.h"

/* Ordered completion times for fixed worker lanes. Admission may tighten for
 * an individual memory class. Its gate is an order statistic of the same set. */
struct lane_node {
    double time;
    unsigned short left, right, size, height;
};
struct lane_schedule {
    struct lane_node nodes[DCC_BROKER_CLIENT_MAX + 1]; /* zero is the empty subtree */
    unsigned root, count;
};

static unsigned lane_update(struct lane_schedule *s, unsigned i)
{
    struct lane_node *n = &s->nodes[i], *l = &s->nodes[n->left], *r = &s->nodes[n->right];
    n->size = 1 + l->size + r->size;
    n->height = 1 + (l->height > r->height ? l->height : r->height);
    return i;
}

static unsigned lane_rotate(struct lane_schedule *s, unsigned i, int left)
{
    struct lane_node *n = &s->nodes[i];
    unsigned j = left ? n->right : n->left;
    struct lane_node *p = &s->nodes[j];
    if (left) { n->right = p->left; p->left = i; }
    else { n->left = p->right; p->right = i; }
    lane_update(s, i);
    return lane_update(s, j);
}

static unsigned lane_balance(struct lane_schedule *s, unsigned i)
{
    struct lane_node *n = &s->nodes[i], *l = &s->nodes[n->left], *r = &s->nodes[n->right];
    if (l->height > r->height + 1) {
        if (s->nodes[l->right].height > s->nodes[l->left].height)
            n->left = lane_rotate(s, n->left, 1);
        return lane_rotate(s, i, 0);
    }
    if (r->height > l->height + 1) {
        if (s->nodes[r->left].height > s->nodes[r->right].height)
            n->right = lane_rotate(s, n->right, 0);
        return lane_rotate(s, i, 1);
    }
    return lane_update(s, i);
}

static unsigned lane_insert(struct lane_schedule *s, unsigned root, unsigned i)
{
    if (!root) return i;
    struct lane_node *n = &s->nodes[root];
    if (s->nodes[i].time < n->time) n->left = lane_insert(s, n->left, i);
    else n->right = lane_insert(s, n->right, i);
    return lane_balance(s, root);
}

static unsigned lane_pop(struct lane_schedule *s, unsigned root, unsigned *first)
{
    struct lane_node *n = &s->nodes[root];
    if (!n->left) { *first = root; return n->right; }
    n->left = lane_pop(s, n->left, first);
    return lane_balance(s, root);
}

static void lane_add(struct lane_schedule *s, unsigned i, double finish)
{
    s->nodes[i] = (struct lane_node){.time = finish, .size = 1, .height = 1};
    s->root = lane_insert(s, s->root, i);
}

static void lane_finish(struct lane_schedule *s, double finish)
{
    unsigned first;
    s->root = lane_pop(s, s->root, &first);
    lane_add(s, first, finish);
}

static double lane_ready(const struct lane_schedule *s, unsigned limit)
{
    unsigned rank = limit < s->count ? s->count - limit : 0;
    unsigned i = s->root;
    for (;;) {
        const struct lane_node *n = &s->nodes[i];
        unsigned left = s->nodes[n->left].size;
        if (rank == left) return n->time;
        if (rank < left) i = n->left;
        else { rank -= left + 1; i = n->right; }
    }
}
#endif
