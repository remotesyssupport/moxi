/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <event.h>
#include "work.h"

bool work_queue_init(work_queue *m, struct event_base *event_base) {
    assert(m != NULL);

    pthread_mutex_init(&m->work_lock, NULL);
    m->work_head = NULL;
    m->work_tail = NULL;

    m->num_items = 0;
    m->tot_sends = 0;
    m->tot_recvs = 0;

    m->event_base = event_base;
    assert(m->event_base != NULL);

    int fds[2];

    if (pipe(fds) == 0) {
        m->recv_fd = fds[0];
        m->send_fd = fds[1];

        event_set(&m->event, m->recv_fd,
                  EV_READ | EV_PERSIST, work_recv, m);
        event_base_set(m->event_base, &m->event);

        if (event_add(&m->event, 0) == 0) {
            return true;
        }
    }

    return false;
}

bool work_send(work_queue *m,
               void (*func)(void *data0, void *data1),
               void *data0, void *data1) {
    assert(m != NULL);
    assert(m->recv_fd >= 0);
    assert(m->send_fd >= 0);
    assert(m->event_base != NULL);
    assert(func != NULL);

    bool rv = false;

    // TODO: Add a free-list of work_items.
    //
    work_item *w = calloc(1, sizeof(work_item));
    if (w != NULL) {
        w->func  = func;
        w->data0 = data0;
        w->data1 = data1;
        w->next  = NULL;

        pthread_mutex_lock(&m->work_lock);

        if (m->work_tail != NULL)
            m->work_tail->next = w;
        m->work_tail = w;
        if (m->work_head == NULL)
            m->work_head = w;

        m->tot_sends++;

        if (write(m->send_fd, "", 1) == 1)
            rv = true;

        pthread_mutex_unlock(&m->work_lock);
    }

    return rv;
}

/* Called by libevent.
 */
void work_recv(int fd, short which, void *arg) {
    work_queue *m = arg;
    assert(m != NULL);
    assert(m->recv_fd == fd);
    assert(m->send_fd >= 0);
    assert(m->event_base != NULL);

    work_item *curr = NULL;
    work_item *next = NULL;

    char buf[1];

    // The lock area includes the read() for safety,
    // as the pipe acts like a cond variable.
    //
    pthread_mutex_lock(&m->work_lock);

    read(fd, buf, 1);

    curr = m->work_head;
    m->work_head = NULL;
    m->work_tail = NULL;

    pthread_mutex_unlock(&m->work_lock);

    uint64_t num_items = 0;

    while (curr != NULL) {
        next = curr->next;
        num_items++;
        curr->func(curr->data0, curr->data1);
        free(curr);
        curr = next;
    }

    if (num_items > 0) {
        pthread_mutex_lock(&m->work_lock);

        m->tot_recvs += num_items;
        m->num_items -= num_items;
        assert(m->num_items >= 0);

        pthread_mutex_unlock(&m->work_lock);
    }
}

void work_collect_init(work_collect *c, int count, void *data) {
    assert(c);

    c->count = count;
    c->data  = data;

    pthread_mutex_init(&c->collect_lock, NULL);
    pthread_cond_init(&c->collect_cond, NULL);
}

void work_collect_wait(work_collect *c) {
    pthread_mutex_lock(&c->collect_lock);
    while (c->count != 0) { // Can't test for > 0, due to -1 on init race.
        pthread_cond_wait(&c->collect_cond, &c->collect_lock);
    }
    pthread_mutex_unlock(&c->collect_lock);
}

void work_collect_count(work_collect *c, int count) {
    pthread_mutex_lock(&c->collect_lock);
    c->count = count;
    if (c->count <= 0)
        pthread_cond_signal(&c->collect_cond);
    pthread_mutex_unlock(&c->collect_lock);
}

void work_collect_one(work_collect *c) {
    pthread_mutex_lock(&c->collect_lock);
    assert(c->count >= 1);
    c->count--;
    if (c->count <= 0)
        pthread_cond_signal(&c->collect_cond);
    pthread_mutex_unlock(&c->collect_lock);
}
