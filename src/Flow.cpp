#include "Flow.h"

namespace Flow
{
	namespace
	{
		std::atomic<float> g_now{ 0.0f };  // read by the event sinks (repeat window), written by the main thread
	}

	float Now() { return g_now.load(std::memory_order_relaxed); }

	void Advance(float a_dt)
	{
		if (a_dt > 0.0f && a_dt < 1.0f) {
			g_now.store(g_now.load(std::memory_order_relaxed) + a_dt, std::memory_order_relaxed);
		}
	}
}
