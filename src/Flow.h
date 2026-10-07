#pragma once

// A session is a C++20 coroutine run on the main thread from the player's per-frame update, so the author's
// Papyrus (Wait / While ... Wait) ports line by line, but each step resumes in the very frame its condition holds:
//   co_await Flow::At(t)            -- session clock reached t
//   co_await Flow::Sleep(s)
//   co_await Flow::Until(pred, s)   -- pred() true (-> true) or s seconds passed (-> false)
// The clock is game time of the player's update (scaled like the animations, stops while the game is paused).

namespace Flow
{
	float Now();
	void  Advance(float a_dt);

	class Task
	{
	public:
		struct promise_type
		{
			std::function<bool()> ready;  // set by the awaiter the task is suspended on

			Task                get_return_object() { return Task{ std::coroutine_handle<promise_type>::from_promise(*this) }; }
			std::suspend_always initial_suspend() noexcept { return {}; }
			std::suspend_always final_suspend() noexcept { return {}; }
			void                return_void() noexcept {}
			void                unhandled_exception() noexcept { logger::error("session: unhandled exception"); }
		};

		Task() = default;
		explicit Task(std::coroutine_handle<promise_type> a_h) :
			_h(a_h) {}
		Task(Task&& a_rhs) noexcept :
			_h(std::exchange(a_rhs._h, {})) {}
		Task& operator=(Task&& a_rhs) noexcept
		{
			if (this != &a_rhs) {
				Reset();
				_h = std::exchange(a_rhs._h, {});
			}
			return *this;
		}
		Task(const Task&) = delete;
		~Task() { Reset(); }

		[[nodiscard]] bool Running() const { return _h && !_h.done(); }

		// resume if the awaited condition holds (the first call starts the task)
		void Step()
		{
			if (!Running()) {
				return;
			}
			auto& p = _h.promise();
			if (p.ready && !p.ready()) {
				return;
			}
			p.ready = nullptr;
			_h.resume();
		}

		void Reset()
		{
			if (_h) {
				_h.destroy();
				_h = {};
			}
		}

	private:
		std::coroutine_handle<promise_type> _h;
	};

	struct At
	{
		float t;

		bool await_ready() const noexcept { return Now() >= t; }
		void await_suspend(std::coroutine_handle<Task::promise_type> a_h)
		{
			a_h.promise().ready = [this] { return Now() >= t; };
		}
		void await_resume() const noexcept {}
	};

	inline At Sleep(float a_seconds) { return At{ Now() + a_seconds }; }

	struct Until
	{
		std::function<bool()> pred;
		float                 timeout{ 1e9f };
		float                 until{ 0.0f };
		bool                  ok{ false };

		bool await_ready()
		{
			until = Now() + timeout;
			ok = pred();
			return ok || Now() >= until;
		}
		void await_suspend(std::coroutine_handle<Task::promise_type> a_h)
		{
			a_h.promise().ready = [this] {
				ok = pred();
				return ok || Now() >= until;
			};
		}
		bool await_resume() const noexcept { return ok; }
	};
}
