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

#include <vector>
#include "Workflow.h"
#include "WFGraphTask.h"

SubTask *WFGraphNode::done()
{
	SeriesWork *series = series_of(this);

	if (!this->user_data)
	{
		/* All in-edges have been settled. Nodes without predecessors
		 * run directly; the others run iff at least one in-edge was
		 * activated. */
		this->user_data = (void *)1;
		this->value = 1;

		if (this->dependencies == 0 || this->activated.load() > 0)
			return series->pop();

		/* Skipped: destroy the node's task without dispatching it and
		 * without calling its callback, then re-dispatch this node once
		 * to settle every out-edge as not activated. */
		this->skipped = true;
		delete series->pop();
		this->graph->count_skipped_node();
		return series->pop();
	}

	if (!this->skipped)
		this->graph->count_executed_node(this->result);

	delete this;
	return series->pop();
}

WFGraphNode::~WFGraphNode()
{
	if (this->user_data)
	{
		bool canceled = series_of(this)->is_canceled();
		bool ran = !this->skipped;

		for (const Edge& edge : this->successors)
		{
			if (canceled)
				series_of(edge.node)->SeriesWork::cancel();

			/* A skipped node activates no out-edge, not even an
			 * unconditional one, but every edge is still settled. */
			edge.node->settle(ran && edge.matches(this->result));
		}
	}
}

WFGraphNode& WFGraphTask::create_graph_node(SubTask *task)
{
	WFGraphNode *node = new WFGraphNode;
	SeriesWork *series = Workflow::create_series_work(node, node, nullptr);

	node->graph = this;
	series->push_back(task);
	this->parallel->add_series(series);
	return *node;
}

void WFGraphTask::dispatch()
{
	SeriesWork *series = series_of(this);

	if (this->parallel)
	{
		series->push_front(this);
		series->push_front(this->parallel);
		this->parallel = NULL;
	}
	else
		this->state = WFT_STATE_SUCCESS;

	this->subtask_done();
}

SubTask *WFGraphTask::done()
{
	SeriesWork *series = series_of(this);

	if (this->state == WFT_STATE_SUCCESS)
	{
		if (this->callback)
			this->callback(this);

		delete this;
	}

	return series->pop();
}

WFGraphTask::~WFGraphTask()
{
	if (this->parallel)
	{
		for (SeriesWork *series : *this->parallel)
			series->unset_last_task();

		this->parallel->dismiss();
	}
}
