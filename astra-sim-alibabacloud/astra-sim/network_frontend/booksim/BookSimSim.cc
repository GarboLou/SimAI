/*
 *Copyright (c) 2024, Alibaba Group;
 *Licensed under the Apache License, Version 2.0 (the "License");
 *you may not use this file except in compliance with the License.
 *You may obtain a copy of the License at

 *   http://www.apache.org/licenses/LICENSE-2.0

 *Unless required by applicable law or agreed to in writing, software
 *distributed under the License is distributed on an "AS IS" BASIS,
 *WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *See the License for the specific language governing permissions and
 *limitations under the License.
*/

#include "BookSimSim.h"
#include "entry.h"

#include "engine/booksim_engine.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace BookSimSim {

namespace {

struct Event {
  EventFn  fn;
  void *   arg;
  uint64_t due;        // for the fidelity check
  uint64_t scheduled;  // when Schedule() was called
};

// Time-ordered, and a list per tick so events registered for the same tick fire
// in registration order.
std::map<uint64_t, std::list<Event> > g_evq;

booksim::Engine * g_engine = NULL;
bool              g_fast_forward = true;
uint64_t          g_violations = 0;

// Bound on how long we tolerate no forward progress before declaring a hang,
// which is what a lost rendezvous looks like from the outside.
const uint64_t kStallLimit = 500000000ull;

} // namespace

void Schedule(uint64_t delta_ns, EventFn fn, void * arg) {
  Event e;
  e.fn        = fn;
  e.arg       = arg;
  e.scheduled = Now();
  e.due       = e.scheduled + delta_ns;
  g_evq[e.due].push_back(e);
}

uint64_t Now() {
  return g_engine ? g_engine->Time() : 0;
}

booksim::Engine * GetEngine() { return g_engine; }

uint64_t ScheduleViolations() { return g_violations; }

void Init(const char * booksim_config, bool fast_forward) {
  std::vector<std::pair<std::string, std::string> > overrides;
  g_engine = booksim::Engine::Create(booksim_config, overrides);
  if(!g_engine) {
    std::cerr << "BookSimSim: failed to create engine from " << booksim_config
              << std::endl;
    exit(-1);
  }
  g_fast_forward = fast_forward;
}

void Run() {
  uint64_t last_progress = 0;
  uint64_t last_flows = 0;

  for(;;) {
    // (1) Fire everything registered for exactly the current tick. A handler
    //     may schedule more at this same tick (Sys does, with delta == 0), so
    //     re-look-up the bucket after every callback rather than iterating it.
    for(;;) {
      uint64_t now = Now();
      std::map<uint64_t, std::list<Event> >::iterator it = g_evq.find(now);
      if(it == g_evq.end() || it->second.empty()) {
        if(it != g_evq.end()) g_evq.erase(it);
        break;
      }
      Event e = it->second.front();
      it->second.pop_front();

      if(e.due != now) {
        // Would mean Sys::call_events looks in the wrong event_queue bucket and
        // silently drops every event in it.
        ++g_violations;
        std::cerr << "BookSimSim: event fired at " << now << " but was due at "
                  << e.due << " (scheduled at " << e.scheduled << ")"
                  << std::endl;
      }
      e.fn(e.arg);
    }

    // (2) Done when there is nothing left to fire and nothing left in flight.
    if(g_evq.empty() && g_engine->IsIdle()) break;

    // (3) Advance. Jumping straight to the next event is safe only while the
    //     network is completely quiescent -- BookSim's channel and router
    //     pipelines are keyed on absolute time. Engine::AdvanceTo asserts it.
    if(g_engine->IsIdle() && !g_evq.empty()) {
      uint64_t next = g_evq.begin()->first;
      if(next > Now()) {
        if(g_fast_forward) {
          g_engine->AdvanceTo(next);
          continue;
        }
      }
    }

    g_engine->Step();

    uint64_t flows = g_engine->FlowsOutstanding();
    if(flows != last_flows || !g_evq.empty()) {
      last_flows = flows;
      last_progress = Now();
    } else if(Now() - last_progress > kStallLimit) {
      std::cerr << "BookSimSim: no progress for " << kStallLimit
                << " cycles; aborting." << std::endl;
      ReportExitState();
      break;
    }
  }
}

void ReportExitState() {
  std::cout << "--- BookSim backend exit state ---" << std::endl;
  std::cout << "  time                  " << Now() << " ns" << std::endl;
  std::cout << "  pending event ticks   " << g_evq.size() << std::endl;
  std::cout << "  engine idle           "
            << ((g_engine && g_engine->IsIdle()) ? "yes" : "NO") << std::endl;
  std::cout << "  flows outstanding     "
            << (g_engine ? g_engine->FlowsOutstanding() : 0) << std::endl;
  std::cout << "  schedule violations   " << g_violations << std::endl;

  // These four are the rendezvous state shared with sim_send/sim_recv. Anything
  // left here is a transfer that was started and never matched -- the failure
  // mode that is otherwise indistinguishable from the workload completing.
  std::cout << "  sentHash              " << sentHash.size() << std::endl;
  std::cout << "  expeRecvHash          " << expeRecvHash.size() << std::endl;
  std::cout << "  recvHash              " << recvHash.size() << std::endl;
  std::cout << "  receiver_pending      " << receiver_pending_queue.size()
            << std::endl;

  if(!expeRecvHash.empty()) {
    std::cout << "  *** " << expeRecvHash.size() << " receive(s) posted but "
              << "never satisfied: a rendezvous was lost. ***" << std::endl;
  }
}

} // namespace BookSimSim
