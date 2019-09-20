#include <iostream>
#include <time.h>
#include <thread>

#include "prometheus.h"

using namespace std;

Prometheus::StatServerLibEv srv;
Prometheus::Metric metric(srv, "test", "test metric", Prometheus::Metric::Type::Counter);

void OnTimer(struct ev_loop* loop, ev_timer* w, int revents)
{
	metric.AddValue(time(NULL));
}

int main()
{
    struct ev_loop *loop = EV_DEFAULT;
    loop = ev_loop_new();

	ev_timer timer;
	ev_timer_init(&timer, OnTimer, 0, 1);
	ev_timer_start(loop, &timer);

	metric.AddLabel("time", "timer");
    srv.Start(loop, 11042);

    ev_run (loop, 0);

    return 0;
}
