// SculptCore — minimal fork/join parallel loop on a persistent thread pool.
#pragma once

#include <cstddef>
#include <functional>

namespace sculpt {

// Calls fn(begin, end) on disjoint sub-ranges covering [0, count), using the
// calling thread plus the pool's workers. Ranges are at least `grain` long;
// small loops (count < 2 * grain) and nested calls run serially on the caller.
// fn must not throw.
void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& fn);

// Worker threads to use (including the caller). 1 = always serial.
// Defaults to the hardware concurrency, capped at 16.
void setParallelThreadCount(unsigned threads);
unsigned parallelThreadCount();

// Stops and joins the workers. Hosts that unload the library (e.g. a DLL
// plugin) must call this before unloading; the pool restarts on next use.
void shutdownParallel();

}  // namespace sculpt
