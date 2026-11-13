/*
  Copyright (c) 2020 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#ifndef _WFGRAPHTASK_H_
#define _WFGRAPHTASK_H_

#include <vector>
#include <utility>
#include <functional>
#include <atomic>
#include "Workflow.h"
#include "WFTask.h"

struct WFGraphSummary
{
	int executed;
	int succeeded;
	int failed;
	int skipped;
};

class WFGraphTask;

class WFGraphNode : protected WFCounterTask
{
public:
	void precede(WFGraphNode& node)
	{
		node.value++;
		node.dependencies++;
		this->successors.push_back(Edge(&node, EDGE_ALWAYS));
	}

	void succeed(WFGraphNode& node)
	{
		node.precede(*this);
	}

	/* Conditional edge: 'node' is activated by this edge only when this
	 * node's business result equals 'success'. */
	void precede_if(WFGraphNode& node, bool success)
	{
		node.value++;
		node.dependencies++;
		this->successors.push_back(Edge(&node, success ? EDGE_ON_SUCCESS
													   : EDGE_ON_FAILURE));
	}

	/* Report the business result of this node. The default result is
	 * success. Typically called in the callback of the node's task,
	 * before the callback returns. */
	void set_result(bool success)
	{
		this->result = success;
	}

protected:
	virtual SubTask *done();

private:
	enum EdgeType
	{
		EDGE_ALWAYS,
		EDGE_ON_SUCCESS,
		EDGE_ON_FAILURE
	};

	struct Edge
	{
		WFGraphNode *node;
		EdgeType type;

		Edge(WFGraphNode *next, EdgeType edge_type) :
			node(next), type(edge_type)
		{
		}

		bool matches(bool result) const
		{
			if (this->type == EDGE_ALWAYS)
				return true;

			return (this->type == EDGE_ON_SUCCESS) == result;
		}
	};

	/* Settle one in-edge. The node is dispatched only after every
	 * in-edge has been settled, activated or not. */
	void settle(bool activated)
	{
		if (activated)
			this->activated++;

		this->WFCounterTask::count();
	}

private:
	std::vector<Edge> successors;
	std::atomic<int> activated;
	unsigned int dependencies;
	WFGraphTask *graph;
	bool result;
	bool skipped;

protected:
	WFGraphNode() : WFCounterTask(0, nullptr)
	{
		this->activated = 0;
		this->dependencies = 0;
		this->graph = NULL;
		this->result = true;
		this->skipped = false;
	}

	virtual ~WFGraphNode();
	friend class WFGraphTask;
};

static inline WFGraphNode& operator --(WFGraphNode& node, int)
{
	return node;
}

static inline WFGraphNode& operator > (WFGraphNode& prec, WFGraphNode& succ)
{
	prec.precede(succ);
	return succ;
}

static inline WFGraphNode& operator < (WFGraphNode& succ, WFGraphNode& prec)
{
	succ.succeed(prec);
	return prec;
}

static inline WFGraphNode& operator --(WFGraphNode& node)
{
	return node;
}

class WFGraphTask : public WFGenericTask
{
public:
	WFGraphNode& create_graph_node(SubTask *task);

public:
	void set_callback(std::function<void (WFGraphTask *)> cb)
	{
		this->callback = std::move(cb);
	}

	/* Valid in the graph's callback. Nodes are already destroyed then,
	 * so the summary is collected on this task while the graph runs. */
	WFGraphSummary get_summary() const
	{
		WFGraphSummary summary;

		summary.executed = this->summary_executed.load();
		summary.succeeded = this->summary_succeeded.load();
		summary.failed = this->summary_failed.load();
		summary.skipped = this->summary_skipped.load();
		return summary;
	}

private:
	void count_executed_node(bool success)
	{
		this->summary_executed++;
		if (success)
			this->summary_succeeded++;
		else
			this->summary_failed++;
	}

	void count_skipped_node()
	{
		this->summary_skipped++;
	}

protected:
	virtual void dispatch();
	virtual SubTask *done();

protected:
	ParallelWork *parallel;
	std::function<void (WFGraphTask *)> callback;
	std::atomic<int> summary_executed;
	std::atomic<int> summary_succeeded;
	std::atomic<int> summary_failed;
	std::atomic<int> summary_skipped;

public:
	WFGraphTask(std::function<void (WFGraphTask *)>&& cb) :
		callback(std::move(cb))
	{
		this->parallel = Workflow::create_parallel_work(nullptr);
		this->summary_executed = 0;
		this->summary_succeeded = 0;
		this->summary_failed = 0;
		this->summary_skipped = 0;
	}

protected:
	virtual ~WFGraphTask();
	friend class WFGraphNode;
};

#endif
