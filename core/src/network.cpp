#include <sys/epoll.h>
#include <poll.h>
#include <vector>
#include <algorithm>
#include <assert.h>
#include <sys/resource.h>

#include "coroutine.h"
#include "coroutine_impl.h"
#include "timewheel.h"
#include "log.h"
#include "hook_helper.h"
#include "timewheel.h"
#include "network_hook.h"
#include "dispatch.h"
#include "gflags/gflags.h"

#include "../../base/incl/list.h"
#include "../../base/incl/tls.h"

DEFINE_int32(epoll_size, 10000, "epoll event size ");


HOOK_DECLARE(
    int, epoll_wait,(int epfd, struct epoll_event *events,
                     int maxevents, int timeout)
);


namespace conet
{

struct poll_ctx_t;

struct poll_wait_item_t
{
    poll_ctx_t * poll_ctx;
    list_head link_to;
    int  pos;
    int fd;
    uint32_t wait_events;
    poll_wait_item_t()
    {
        INIT_LIST_HEAD(&link_to);
        poll_ctx = NULL;
        pos = 0;
        wait_events = 0;
        this->fd = 0;
    }

    explicit
    poll_wait_item_t(int fd)
    {
        INIT_LIST_HEAD(&link_to);
        poll_ctx = NULL;
        pos = 0;
        wait_events = 0;
        this->fd = fd;
    }
};

struct poll_wait_item_mgr_t
{
    poll_wait_item_t ** wait_items;
    int size;

    static int default_size;
    static 
    int get_default_size()
    {
        if (default_size == 0) {
            struct rlimit rl;
            int ret = 0;
            ret = getrlimit( RLIMIT_NOFILE, &rl);
            if (ret) {
                default_size = 100000;
            } else {
                default_size = rl.rlim_max;
            }
        }
        return default_size;
    }

    poll_wait_item_mgr_t() 
    {
        this->size = get_default_size(); 
        wait_items = (poll_wait_item_t **) new poll_wait_item_t *[size+1];
        memset(wait_items, 0, (size+1)*sizeof(void *));
    }

    int expand(int need_size)
    {
        int new_size = size;
        while (new_size <= need_size) {
            new_size +=10000;
        }

        poll_wait_item_t **ws = (poll_wait_item_t **) new poll_wait_item_t *[new_size+1];
        memset(ws, 0, (new_size+1)*sizeof(void *));
        memcpy(ws, this->wait_items, (this->size+1) * sizeof(void *) ); 

        delete [] this->wait_items;
        this->wait_items = ws;
        this->size = new_size;
        return new_size;
    }

    ~poll_wait_item_mgr_t()
    {
        for(int i=0;i<= this->size; ++i) 
        {
            if (this->wait_items[i]) {
                delete this->wait_items[i];
            }
        }
        delete [] (this->wait_items);
    }
};

int poll_wait_item_mgr_t::default_size = 0;

__thread  poll_wait_item_mgr_t * g_wait_item_mgr = NULL;
CONET_DEF_TLS_VAR_HELP_DEF(g_wait_item_mgr);

poll_wait_item_t * get_wait_item(int fd) 
{
    if (fd < 0) {
        LOG(FATAL)<<"error [fd:"<<fd<<"]";
        abort();
        return NULL;
    }

    poll_wait_item_mgr_t *mgr = TLS_GET(g_wait_item_mgr);

    if ( fd >= mgr->size)
    {
        mgr->expand(fd);
    }

    poll_wait_item_t *wait_item = mgr->wait_items[fd];
    if (NULL == wait_item )
    {
        wait_item = new poll_wait_item_t(fd);
        mgr->wait_items[fd] = wait_item;
    }
    return wait_item;
}

struct poll_ctx_t
{
    struct pollfd *fds;
    nfds_t nfds;

    int all_event_detach;

    int num_raise;

    int retcode;

    coroutine_t * coroutine;

    timeout_handle_t timeout_ctl;

    list_head to_dispatch; // fd 有事件的时候， 把这个poll加入到 分发中
    int timeout;

    void init(pollfd *fds, nfds_t nfds, int epoll_fd, int timeout);
};


epoll_ctx_t * get_epoll_ctx();


void close_fd_notify_poll(int fd)
{
    poll_wait_item_t *wait_item = get_wait_item(fd);
    if (wait_item && wait_item->wait_events)  
    {
        epoll_ctx_t *ep_ctx = get_epoll_ctx();
        uint32_t events = wait_item->wait_events;
        wait_item->wait_events = 0;
        epoll_event ev;
        ev.events = 0;
        ev.data.ptr = wait_item;
        int ret = epoll_ctl(ep_ctx->m_epoll_fd, fd, EPOLL_CTL_DEL, &ev);
        if (ret) {
            LOG_SYS_CALL(epoll_ctl, ret)<<" epoll_ctl_del [fd:"<<fd<<"] [events:"<<events<<"]";
        }
    }
}

void clear_invalid_event(poll_wait_item_t *wait_item, uint32_t events, int epoll_fd)
{
    int ret = 0;
    int fd = wait_item->fd;
    uint32_t wait_events = wait_item->wait_events;
    epoll_event ev;
    ev.events = wait_events & (~events);
    ev.data.ptr = wait_item;
    if (ev.events) {
        wait_item->wait_events= ev.events;
        ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd,  &ev);
        if (ret) {
            LOG_SYS_CALL(epoll_ctl, ret)<<" epoll_ctl_mod [fd:"<<fd<<"]";
        }

    } else {
        wait_item->wait_events= 0;
        ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd,  &ev);
        if (ret) {
            LOG_SYS_CALL(epoll_ctl, ret)<<" epoll_ctl_del [fd:"<<fd<<"]";
        }
    }
    return;
}

static
inline uint32_t epoll_event2poll( uint32_t events )
{
    uint32_t e = 0; 
    if( events & EPOLLIN ) 	e |= POLLIN;
    if( events & EPOLLOUT ) e |= POLLOUT;
    if( events & EPOLLHUP ) e |= POLLHUP;
    if( events & EPOLLERR ) e |= POLLERR;
    return e;
}

static
inline uint32_t poll_event2epoll(uint32_t  events )
{
    uint32_t e = 0;
    if( events & POLLIN ) 	e |= EPOLLIN;
    if( events & POLLOUT )  e |= EPOLLOUT;
    if( events & POLLHUP ) 	e |= EPOLLHUP;
    if( events & POLLERR )	e |= EPOLLERR;
    return e;
}


void fd_notify_events_to_poll(poll_wait_item_t *wait_item, uint32_t events, list_head *dispatch, int epoll_fd)
{
    poll_ctx_t *poll_ctx = wait_item->poll_ctx;
    if (NULL == poll_ctx)
    { // poll 已经返回了， 这个事件可以清除
        clear_invalid_event(wait_item, events, epoll_fd);
        return;
    }

    int fd = wait_item->fd;
    int nfds = (int) poll_ctx->nfds;
    int pos = wait_item->pos;
    if ( (pos < 0) || ( nfds <= pos) ) {
        clear_invalid_event(wait_item, events, epoll_fd);
        LOG(ERROR)<<"error [fd:"<<fd<<"] ctx [pos:"<<pos<<"]";
        abort();
        return;
    }

    struct pollfd * fds = poll_ctx->fds;
    // set reachable events
    uint32_t mask = poll_event2epoll(fds[pos].events);
    uint32_t revents = (mask & events);

    if (revents) {
        //epoll 事件必须转换为 poll 的事件
        fds[pos].revents |= epoll_event2poll(revents);
        if (poll_ctx->timeout >=0) 
        {
            cancel_timeout(&poll_ctx->timeout_ctl);
        }
        //add to dispatch
        list_move_tail(&poll_ctx->to_dispatch, dispatch);
        ++poll_ctx->num_raise;
    } else {
        clear_invalid_event(wait_item, events, epoll_fd);
    }
}

epoll_ctx_t *create_epoll(int event_size);





void init_epoll_ctx(epoll_ctx_t *self, int size)
{
    self->m_epoll_size = size;
    self->m_epoll_events = new epoll_event[size];
    memset(self->m_epoll_events, 0, sizeof(epoll_event) *size);
    self->m_epoll_fd = epoll_create(size);
    self->wait_num = 0;
    return;
}

void poll_ctx_timeout_proc(void *arg);

void  poll_ctx_t::init(pollfd *fds, nfds_t nfds, int epoll_fd, int timeout)
{
    this->fds = fds;
    this->nfds = nfds;
    this->num_raise = 0;
    this->all_event_detach = 0;
    this->coroutine = NULL;
    this->retcode = 0;
    this->timeout = timeout;

    if (timeout >=0) 
    {
        init_timeout_handle(&this->timeout_ctl, poll_ctx_timeout_proc, this);
        set_timeout(&this->timeout_ctl, timeout);
    }

    INIT_LIST_HEAD(&this->to_dispatch);

    int ret  = 0;
    for(int i=0; i< (int)nfds; ++i) 
    {
        int fd= fds[i].fd;
        fds[i].revents = 0;
        if( fd < 0) {
            LOG(FATAL)<<"error fd, [fd:"<<fd<<"]";
            abort();
        }

        poll_wait_item_t *wait_item = get_wait_item(fd);
        if (NULL == wait_item) {
            LOG(FATAL)<<"get wait item failed, [fd:"<<fd<<"]";
            abort();
        }

        if (wait_item->poll_ctx != NULL) {
            LOG(FATAL)<<"[fd:"<<fd<<", has been polled by other";
            abort();
        }

        wait_item->poll_ctx = this;
        wait_item->pos = i;

        uint32_t wait_events = wait_item->wait_events;
        epoll_event ev;
        ev.events = poll_event2epoll( fds[i].events);
        ev.data.ptr = wait_item;
        if (wait_events) { // 已经设置过EPOLL事件
            uint32_t events = ev.events;
            events |= wait_events;
            if (events != wait_events) { // 有变化， 修改事件
                ev.events = events;
                wait_item->wait_events = events;
                ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd,  &ev);
                if (ret) {
                    LOG_SYS_CALL(epoll_ctl, ret)<<" epoll_ctl_mod [fd:"<<fd<<"]";
                }
            } 
        } else {
            // 新句柄
            wait_item->wait_events = ev.events;
            ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd,  &ev);
            if (ret) {
                LOG_SYS_CALL(epoll_ctl, ret)<<" epoll_ctl_add [fd:"<<fd<<"]";
            }
        }
    }
}

void destruct_poll_ctx(poll_ctx_t *self, epoll_ctx_t * epoll_ctx)
{
    for(int i=0; i< (int)self->nfds; ++i)
    {
        poll_wait_item_t *wait_item = get_wait_item(self->fds[i].fd);
        wait_item->poll_ctx = NULL;
    }
}

int proc_netevent(epoll_ctx_t * epoll_ctx, int timeout)
{
    if (!epoll_ctx) return -1;

    int ret = 0;
    int cnt = 0;
    epoll_event *evs = epoll_ctx->m_epoll_events;
    int ep_size = epoll_ctx->m_epoll_size;
    int ep_fd = epoll_ctx->m_epoll_fd;

    ret = _(epoll_wait)(ep_fd, evs, ep_size, timeout);
    if (ret <0 ) {
        // epoll_wait failed;
        if (errno != 4) {
            LOG_SYS_CALL(epoll_wait, ret);
        }
        return 0;
    }
    if (ret == 0) {
        return ret;
    }
    int ev_num = ret;

    if (ev_num > ep_size) ev_num = ep_size;

    LIST_HEAD(dispatch);

    for (int i=0; i<ev_num; ++i) {
        poll_wait_item_t * wait_item = (poll_wait_item_t *)(evs[i].data.ptr);
        uint32_t events = evs[i].events;
        fd_notify_events_to_poll(wait_item, events, &dispatch, ep_fd);
    }


    poll_ctx_t *ctx = NULL, *next = NULL;

    list_for_each_entry_safe(ctx, next, &dispatch, to_dispatch)
    {
        list_del_init(&ctx->to_dispatch);
        if (ctx->coroutine) {
            cnt++;
            resume(ctx->coroutine);
        }
    }
    return cnt;
}

int proc_netevent(int timeout)
{
    epoll_ctx_t * epoll_ctx =  get_epoll_ctx();
    return proc_netevent(epoll_ctx, timeout);
}

int task_proc(void *arg)
{
    return proc_netevent((epoll_ctx_t *) arg, -1);
}

epoll_ctx_t *create_epoll(int event_size)
{
    epoll_ctx_t * p = new epoll_ctx_t;
    init_epoll_ctx(p, event_size);
    registry_task(task_proc, p);
    return p;
}

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
    epoll_ctx_t *epoll_ctx  = get_epoll_ctx();

    poll_ctx_t poll_ctx;

    poll_ctx.init(fds, nfds, epoll_ctx->m_epoll_fd, timeout);

    poll_ctx.coroutine = current_coroutine();

    ++ epoll_ctx->wait_num;
    
    yield();

    -- epoll_ctx->wait_num;

    destruct_poll_ctx(&poll_ctx, epoll_ctx);

    switch (poll_ctx.retcode)
    {
        case 0: // 有事件
            return poll_ctx.num_raise;
        case 1: // 超时
            return 0;
        case 2: // 出错了
            return -1;
        default:
            return -1;
    }
}


void free_epoll(epoll_ctx_t *ep)
{
    delete [] ep->m_epoll_events;
    delete ep;
}

int get_epoll_pend_task_num() 
{
    return get_epoll_ctx()->wait_num;
}

__thread epoll_ctx_t * g_epoll_ctx = NULL;

CONET_DEF_TLS_GET(g_epoll_ctx, create_epoll(FLAGS_epoll_size), free_epoll);

epoll_ctx_t * get_epoll_ctx()
{
    return TLS_GET(g_epoll_ctx);
}

void poll_ctx_timeout_proc(void *arg)
{
    poll_ctx_t *self = (poll_ctx_t *)(arg);
    self->retcode = 1;

    if (self->coroutine) 
    {
        resume(self->coroutine);
    }
    return ;
}


}

