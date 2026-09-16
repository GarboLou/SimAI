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

#ifndef __BOOKSIMSIM_H__
#define __BOOKSIMSIM_H__

#include <cstdint>
#include <list>
#include <map>

namespace booksim { class Engine; }

// The simulation driver: owns the BookSim engine, the clock, and the event
// queue that ASTRA-sim's Sys layer schedules onto.
//
// Deliberately NOT modelled on AnaSim (network_frontend/analytical/AnaSim.cc).
// That uses a FIFO std::queue plus `while (task.time != tick) tick++;`, which
// spins forever the moment an event is inserted out of timestamp order -- which
// happens as soon as two ranks interleave. It only survives in the analytical
// build because -DANALYTI stops any collective from ever being issued.
namespace BookSimSim {

typedef void (*EventFn)(void * arg);

// Schedule fn(arg) to fire exactly `delta_ns` from now.
//
// "Exactly" is load-bearing. Sys::try_register_event puts the callable into
// event_queue[boostedTick() + cycles] and only calls sim_schedule when that
// bucket is new; Sys::call_events then drains event_queue[boostedTick()] and
// erases it. If the callback fires even one tick late the bucket key no longer
// matches and the entire bucket is dropped silently.
void Schedule(uint64_t delta_ns, EventFn fn, void * arg);

// Current simulated time in ns. One BookSim cycle is one ns.
uint64_t Now();

booksim::Engine * GetEngine();

// Build the engine from a BookSim config. Must be called before Run().
void Init(const char * booksim_config, bool fast_forward);

// Interleave event dispatch with BookSim cycles until both are exhausted.
void Run();

// Diagnostics printed at exit. A non-empty rendezvous table here means a
// completion was lost, which otherwise looks exactly like a clean finish.
void ReportExitState();

// Verifies every scheduled event fired at exactly now+delta. Returns the number
// of violations (always 0 if the queue is behaving).
uint64_t ScheduleViolations();

} // namespace BookSimSim

#endif
