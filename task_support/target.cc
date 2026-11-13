#include <workflow/WFTaskFactory.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFFacilities.h>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>

static void check(bool ok, const char *expr, int line)
{
	if (!ok)
	{
		std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
		std::exit(1);
	}
}

#define CHECK(expr) check((expr), #expr, __LINE__)

/* A SubTask that counts how often it is dispatched, finished (done)
 * and destroyed, with optional hooks. Used to verify that skipped
 * nodes never dispatch their task but still destroy it exactly once. */
struct Probe
{
	std::atomic<int> dispatched{0};
	std::atomic<int> finished{0};
	std::atomic<int> destroyed{0};
};

class ProbeTask : public WFGenericTask
{
public:
	Probe *probe;
	std::function<void()> on_dispatch;
	std::function<void()> on_done;

	explicit ProbeTask(Probe *p) : probe(p) { }

protected:
	virtual void dispatch()
	{
		this->probe->dispatched++;
		if (this->on_dispatch)
			this->on_dispatch();
		this->subtask_done();
	}

	virtual SubTask *done()
	{
		this->probe->finished++;
		if (this->on_done)
			this->on_done();
		return WFGenericTask::done();
	}

	virtual ~ProbeTask()
	{
		this->probe->destroyed++;
	}
};

/* C++11 barrier. The timeout is only a watchdog against deadlocks;
 * the rendezvous itself is pure condition-variable synchronization. */
class Barrier
{
public:
	explicit Barrier(int parties) :
		parties_(parties), arrived_(0), generation_(0)
	{
	}

	bool arrive_and_wait()
	{
		std::unique_lock<std::mutex> lock(this->mutex_);
		int gen = this->generation_;

		if (++this->arrived_ == this->parties_)
		{
			this->arrived_ = 0;
			this->generation_++;
			this->cv_.notify_all();
			return true;
		}

		return this->cv_.wait_for(lock, std::chrono::seconds(30),
								  [&] { return this->generation_ != gen; });
	}

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	int parties_;
	int arrived_;
	int generation_;
};

struct GraphRun
{
	WFGraphSummary summary;
	int callbacks;
};

/* Build a graph, run it to completion and capture the summary that
 * the graph callback observes. */
static GraphRun run_graph(const std::function<void(WFGraphTask *)>& build)
{
	GraphRun run;
	WFFacilities::WaitGroup wait(1);

	run.callbacks = 0;
	WFGraphTask *g = WFTaskFactory::create_graph_task([&](WFGraphTask *gt) {
		run.summary = gt->get_summary();
		run.callbacks++;
		wait.done();
	});
	build(g);
	g->start();
	wait.wait();
	return run;
}

/* Incremental interface succeeds -> process the delta directly; on
 * failure fall back to the full interface; both paths join into one
 * publish. A real local WFHttpServer proves via request counts that
 * the unselected interface is never called. */
static void conditional_http()
{
	std::atomic<int> delta_hits{0};
	std::atomic<int> full_hits{0};

	WFHttpServer server([&](WFHttpTask *t) {
		std::string path = t->get_req()->get_request_uri();
		const char *code = "200";
		std::string body;

		if (path == "/delta")
		{
			delta_hits++;
			body = "delta-result";
		}
		else if (path == "/delta_fail")
		{
			delta_hits++;
			code = "500";
			body = "delta-unavailable";
		}
		else if (path == "/full")
		{
			full_hits++;
			body = "full-result";
		}
		else
		{
			code = "404";
			body = "not-found";
		}

		t->get_resp()->set_http_version("HTTP/1.1");
		t->get_resp()->set_status_code(code);
		t->get_resp()->set_reason_phrase(body == "delta-unavailable" ? "Error" : "OK");
		t->get_resp()->append_output_body(body);
	});
	CHECK(server.start("127.0.0.1", 0) == 0);

	sockaddr_in addr{};
	socklen_t len = sizeof(addr);
	CHECK(server.get_listen_addr((sockaddr *)&addr, &len) == 0);
	std::string root = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));

	for (int scenario = 0; scenario < 2; scenario++)
	{
		bool delta_ok = (scenario == 0);
		std::atomic<int> process_delta{0};
		std::atomic<int> process_full{0};
		std::atomic<int> publish{0};
		std::string delta_body;
		std::string full_body;
		WFGraphNode *delta_node = nullptr;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &delta = g->create_graph_node(WFTaskFactory::create_http_task(
				root + (delta_ok ? "/delta" : "/delta_fail"), 0, 0,
				[&](WFHttpTask *t) {
					CHECK(t->get_state() == WFT_STATE_SUCCESS);
					std::string code = t->get_resp()->get_status_code();
					const void *data;
					size_t n;
					t->get_resp()->get_parsed_body(&data, &n);
					delta_body.assign((const char *)data, n);
					if (code != "200")
						delta_node->set_result(false);
				}));
			delta_node = &delta;

			auto &proc_delta = g->create_graph_node(
				WFTaskFactory::create_timer_task(0, [&](WFTimerTask *) {
					process_delta++;
					CHECK(delta_body == "delta-result");
				}));

			auto &full = g->create_graph_node(WFTaskFactory::create_http_task(
				root + "/full", 0, 0,
				[&](WFHttpTask *t) {
					CHECK(t->get_state() == WFT_STATE_SUCCESS);
					CHECK(std::string(t->get_resp()->get_status_code()) == "200");
					const void *data;
					size_t n;
					t->get_resp()->get_parsed_body(&data, &n);
					full_body.assign((const char *)data, n);
				}));

			auto &proc_full = g->create_graph_node(
				WFTaskFactory::create_timer_task(0, [&](WFTimerTask *) {
					process_full++;
					CHECK(full_body == "full-result");
				}));

			auto &pub = g->create_graph_node(
				WFTaskFactory::create_timer_task(0, [&](WFTimerTask *) {
					publish++;
					CHECK(process_delta + process_full == 1);
				}));

			delta.precede_if(proc_delta, true);
			delta.precede_if(full, false);
			proc_delta.precede(pub);
			full.precede(proc_full);
			proc_full.precede(pub);
		});

		CHECK(run.callbacks == 1);
		CHECK(publish == 1);
		if (delta_ok)
		{
			/* The full interface was never requested. */
			CHECK(delta_hits == 1 && full_hits == 0);
			CHECK(process_delta == 1 && process_full == 0);
			CHECK(delta_body == "delta-result");
			CHECK(run.summary.executed == 3);
			CHECK(run.summary.succeeded == 3);
			CHECK(run.summary.failed == 0);
			CHECK(run.summary.skipped == 2);
		}
		else
		{
			/* Delta reported failure; the full path ran instead. */
			CHECK(delta_hits == 2 && full_hits == 1);
			CHECK(process_delta == 0 && process_full == 1);
			CHECK(delta_body == "delta-unavailable");
			CHECK(full_body == "full-result");
			CHECK(run.summary.executed == 4);
			CHECK(run.summary.succeeded == 3);
			CHECK(run.summary.failed == 1);
			CHECK(run.summary.skipped == 1);
		}
		CHECK(run.summary.executed ==
			  run.summary.succeeded + run.summary.failed);
	}

	server.stop();
	std::puts("conditional_http: PASS "
			  "delta-success(full_hits=0) delta-failure(full fallback) "
			  "join=publish-once");
}

static void join_reachability()
{
	/* 1. Join with one matched and one unmatched edge. A condition
	 * variable forces A to finish before B is even allowed to run;
	 * the join must not fire before B's edge is settled, must fire
	 * exactly once afterwards. */
	{
		Probe pa, pj;
		std::mutex mu;
		std::condition_variable cv;
		bool a_done = false;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto *ta = new ProbeTask(&pa);
			ta->on_done = [&] {
				{
					std::lock_guard<std::mutex> lock(mu);
					a_done = true;
				}
				cv.notify_all();
			};
			auto &a = g->create_graph_node(ta);
			auto &b = g->create_graph_node(WFTaskFactory::create_go_task(
				"join_ordered", [&] {
					std::unique_lock<std::mutex> lock(mu);
					cv.wait(lock, [&] { return a_done; });
					lock.unlock();
					CHECK(pa.finished == 1);
					/* B's edge is not settled yet, so the join
					 * cannot have been dispatched. */
					CHECK(pj.dispatched == 0);
				}));
			auto &j = g->create_graph_node(new ProbeTask(&pj));

			a.precede_if(j, true);   /* matched: A succeeds */
			b.precede_if(j, false);  /* unmatched: B succeeds too */
		});

		CHECK(run.callbacks == 1);
		CHECK(pj.dispatched == 1 && pj.finished == 1 && pj.destroyed == 1);
		CHECK(run.summary.executed == 3);
		CHECK(run.summary.succeeded == 3);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 0);
	}

	/* 2. Neither edge matches: the join is skipped, and an
	 * unconditional edge from the skipped join does not activate
	 * its own successor either. */
	{
		Probe pa, pb, pj, pk;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &a = g->create_graph_node(new ProbeTask(&pa));
			auto &b = g->create_graph_node(new ProbeTask(&pb));
			auto &j = g->create_graph_node(new ProbeTask(&pj));
			auto &k = g->create_graph_node(new ProbeTask(&pk));

			a.precede_if(j, false);
			b.precede_if(j, false);
			j.precede(k);
		});

		CHECK(run.callbacks == 1);
		CHECK(pa.dispatched == 1 && pb.dispatched == 1);
		CHECK(pj.dispatched == 0 && pj.finished == 0 && pj.destroyed == 1);
		CHECK(pk.dispatched == 0 && pk.finished == 0 && pk.destroyed == 1);
		CHECK(run.summary.executed == 2);
		CHECK(run.summary.succeeded == 2);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 2);
	}

	/* 3. A skipped chain re-joins a reachable path: C waits for the
	 * skipped B (edge settled, not activated) and for D (activated),
	 * then runs exactly once. */
	{
		Probe pa, pb, pc, pd;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &a = g->create_graph_node(new ProbeTask(&pa));
			auto &b = g->create_graph_node(new ProbeTask(&pb));
			auto &c = g->create_graph_node(new ProbeTask(&pc));
			auto &d = g->create_graph_node(new ProbeTask(&pd));

			a.precede_if(b, false); /* A succeeds: B is skipped */
			b.precede(c);           /* skipped source: not activated */
			d.precede(c);           /* D succeeds: activated */
		});

		CHECK(run.callbacks == 1);
		CHECK(pb.dispatched == 0 && pb.destroyed == 1);
		CHECK(pc.dispatched == 1 && pc.finished == 1);
		CHECK(run.summary.executed == 3);
		CHECK(run.summary.succeeded == 3);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 1);
	}

	/* 4. Two predecessors released simultaneously rendezvous on a
	 * barrier and then race to settle the join's three edges (two of
	 * them between the same pair of nodes). Exactly-once dispatch. */
	{
		Barrier barrier(2);
		Probe pj;
		std::atomic<int> go_ran{0};

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &a = g->create_graph_node(WFTaskFactory::create_go_task(
				"join_race_a", [&] {
					go_ran++;
					CHECK(barrier.arrive_and_wait());
				}));
			auto &b = g->create_graph_node(WFTaskFactory::create_go_task(
				"join_race_b", [&] {
					go_ran++;
					CHECK(barrier.arrive_and_wait());
				}));
			auto &j = g->create_graph_node(new ProbeTask(&pj));

			a.precede_if(j, true);   /* matched */
			a.precede_if(j, false);  /* not matched */
			b.precede(j);            /* unconditional: matched */
		});

		CHECK(run.callbacks == 1);
		CHECK(go_ran == 2);
		CHECK(pj.dispatched == 1 && pj.finished == 1 && pj.destroyed == 1);
		CHECK(run.summary.executed == 3);
		CHECK(run.summary.succeeded == 3);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 0);
	}

	/* 5. Two conditional edges between the same pair settle edge by
	 * edge: on failure exactly one of them activates the join. */
	{
		Probe pa, pj;
		WFGraphNode *a_node = nullptr;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto *ta = new ProbeTask(&pa);
			ta->on_done = [&] { a_node->set_result(false); };
			auto &a = g->create_graph_node(ta);
			a_node = &a;
			auto &j = g->create_graph_node(new ProbeTask(&pj));

			a.precede_if(j, false);  /* matched: A fails */
			a.precede_if(j, true);   /* not matched */
		});

		CHECK(run.callbacks == 1);
		CHECK(pj.dispatched == 1 && pj.finished == 1);
		CHECK(run.summary.executed == 2);
		CHECK(run.summary.succeeded == 1);
		CHECK(run.summary.failed == 1);
		CHECK(run.summary.skipped == 0);
	}

	std::puts("join_reachability: PASS "
			  "matched+unmatched both-unmatched skipped-chain-rejoin "
			  "multi-edge-race per-edge-settle");
}

static void ownership()
{
	/* 1. Skipped node: its task is destroyed exactly once, never
	 * dispatched, never called back. Skipped is not failed. */
	{
		Probe pa, pb;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &a = g->create_graph_node(new ProbeTask(&pa));
			auto &b = g->create_graph_node(new ProbeTask(&pb));

			a.precede_if(b, false); /* A succeeds: B is skipped */
		});

		CHECK(run.callbacks == 1);
		CHECK(pa.dispatched == 1 && pa.finished == 1 && pa.destroyed == 1);
		CHECK(pb.dispatched == 0 && pb.finished == 0 && pb.destroyed == 1);
		CHECK(run.summary.executed == 1);
		CHECK(run.summary.succeeded == 1);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 1);
	}

	/* 2. Normal execution: dispatch, callback and destruction once. */
	{
		Probe p;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			g->create_graph_node(new ProbeTask(&p));
		});

		CHECK(run.callbacks == 1);
		CHECK(p.dispatched == 1 && p.finished == 1 && p.destroyed == 1);
		CHECK(run.summary.executed == 1);
		CHECK(run.summary.succeeded == 1);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 0);
	}

	/* 3. Empty graph: the callback still happens exactly once. */
	{
		GraphRun run = run_graph([](WFGraphTask *) { });

		CHECK(run.callbacks == 1);
		CHECK(run.summary.executed == 0);
		CHECK(run.summary.succeeded == 0);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 0);
	}

	/* 4. Legacy unconditional diamond: unchanged behavior. */
	{
		Probe pm, pf, pd, pj;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto &mode = g->create_graph_node(new ProbeTask(&pm));
			auto &full = g->create_graph_node(new ProbeTask(&pf));
			auto &delta = g->create_graph_node(new ProbeTask(&pd));
			auto &join = g->create_graph_node(new ProbeTask(&pj));

			mode.precede(full);
			mode.precede(delta);
			full.precede(join);
			delta.precede(join);
		});

		CHECK(run.callbacks == 1);
		CHECK(pm.dispatched == 1 && pf.dispatched == 1 &&
			  pd.dispatched == 1 && pj.dispatched == 1);
		CHECK(pm.destroyed == 1 && pf.destroyed == 1 &&
			  pd.destroyed == 1 && pj.destroyed == 1);
		CHECK(run.summary.executed == 4);
		CHECK(run.summary.succeeded == 4);
		CHECK(run.summary.failed == 0);
		CHECK(run.summary.skipped == 0);
	}

	/* 5. A whole unreachable subgraph: the root reports failure, the
	 * success-only child and its unconditional descendant are both
	 * skipped, the failure branch runs, and the graph callback still
	 * happens exactly once. */
	{
		Probe proot, pchild, pgrand, ptail;
		WFGraphNode *root_node = nullptr;

		GraphRun run = run_graph([&](WFGraphTask *g) {
			auto *troot = new ProbeTask(&proot);
			troot->on_done = [&] { root_node->set_result(false); };
			auto &root = g->create_graph_node(troot);
			root_node = &root;
			auto &child = g->create_graph_node(new ProbeTask(&pchild));
			auto &grand = g->create_graph_node(new ProbeTask(&pgrand));
			auto &tail = g->create_graph_node(new ProbeTask(&ptail));

			root.precede_if(child, true);  /* not matched */
			child.precede(grand);          /* skipped source */
			root.precede_if(tail, false);  /* matched */
		});

		CHECK(run.callbacks == 1);
		CHECK(proot.dispatched == 1 && proot.destroyed == 1);
		CHECK(pchild.dispatched == 0 && pchild.destroyed == 1);
		CHECK(pgrand.dispatched == 0 && pgrand.destroyed == 1);
		CHECK(ptail.dispatched == 1 && ptail.destroyed == 1);
		CHECK(run.summary.executed == 2);
		CHECK(run.summary.succeeded == 1);
		CHECK(run.summary.failed == 1);
		CHECK(run.summary.skipped == 2);
	}

	std::puts("ownership: PASS "
			  "skip-destroy-once normal empty-graph diamond unreachable-subgraph");
}

int main(int argc, char *argv[])
{
	bool all = (argc == 1);

	if (all || (argc > 1 && std::string(argv[1]) == "conditional_http"))
		conditional_http();
	if (all || (argc > 1 && std::string(argv[1]) == "join_reachability"))
		join_reachability();
	if (all || (argc > 1 && std::string(argv[1]) == "ownership"))
		ownership();

	std::puts("ALL TARGET TESTS PASSED");
	return 0;
}
