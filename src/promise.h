#pragma once


#include <stdint.h>
#include <string.h>

#include <memory>
#include <type_traits>


#define ASSERT(x) do while (false)





struct Void {};


struct PromiseState {
	enum Type : uint8_t {
		Pending,

		Success,
		Failure,

		IsRoot = 1 << 7,

		Mask = IsRoot
	};
};





template <typename T>
struct Undecorated {
	typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type type;
};


template <typename T>
struct Move {
	T operator() (T && value) {
		return value;
	}
};


class PromiseImplBase {
	using State = std::underlying_type<PromiseState::Type>::type;

public:
	virtual ~PromiseImplBase() {}

	virtual void opaqueResolveSuccess(void *) {
		ASSERT(false); // Logic error.
	}

	virtual void opaqueResolveFailure(void *) {
		ASSERT(false); // Logic error.
	}

	void setIsRoot()
	{
		state_ |= PromiseState::IsRoot;
	}

	bool isRoot() const
	{
		return (state_ & PromiseState::IsRoot) != 0;
	}

	bool isPending() const
	{
		return getState() == PromiseState::Pending;
	}

	PromiseState::Type getState() const
	{
		return static_cast<PromiseState::Type>(state_ & ~PromiseState::Mask);
	}

	void setState(PromiseState::Type state)
	{
		ASSERT((state & PromiseState::Mask) == 0);
		State mask = state_ & PromiseState::Mask;
		state_ = state | mask;
	}

	void assignBase(PromiseImplBase const & other)
	{
		child_ = other.child_;
		state_ = other.state_;
	}

protected:
	PromiseImplBase * child_ = nullptr;
private:
	State state_ = PromiseState::Pending;
};


template <typename Success, typename Failure>
class PromiseImpl : public PromiseImplBase {
	template <typename S, typename F>
	friend class PromiseImpl;

	PromiseImpl(PromiseImpl const &) = delete;
	void operator= (PromiseImpl const &) = delete;

public:
	PromiseImpl() = default;

	void operator= (PromiseImpl && other)
	{
		assignBase(other);
		success_ = std::move(other.success_);
		failure_ = std::move(other.failure_);
	}

	template <typename S, typename F, typename ToS, typename ToF>
	struct ChildImpl : PromiseImpl<S, F> {
	protected:
		virtual void opaqueResolveSuccess(void * opaque) override {
			auto & success = *static_cast<Success *>(opaque);
			this->resolveSuccess(map_success_(std::move(success)));
		}

		virtual void opaqueResolveFailure(void * opaque) override {
			auto & failure = *static_cast<Failure *>(opaque);
			this->resolveFailure(map_failure_(std::move(failure)));
		}

	public:
		ChildImpl(ToS && map_success, ToF && map_failure)
			: map_success_(std::move(map_success))
			, map_failure_(std::move(map_failure))
		{}

	private:
		ToS map_success_;
		ToF map_failure_;
	};

	template <typename S, typename F, typename ToS, typename ToF>
	PromiseImpl<S, F> * then(
		ToS && map_success,
		ToF && map_failure)
	{
		ASSERT(!child_);

		auto * child = new ChildImpl<S, F, ToS, ToF>(std::move(map_success), std::move(map_failure));
		child_ = child;

		if (!isPending()) {
			auto state = getState();

			switch (state) {
				case PromiseState::Success: {
					child_->opaqueResolveSuccess(&success_);
				} break;

				case PromiseState::Failure: {
					child_->opaqueResolveFailure(&failure_);
				} break;

			}

			destruct();
		}

		return child;
	}

	void thenTerminate()
	{
		child_ = this;
	}

	void resolveSuccess(Success && success)
	{
		ASSERT(isPending());

		if (child_) {
			if (child_ != this) {
				child_->opaqueResolveSuccess(&success);
			}
			destruct();
		}
		else {
			setState(PromiseState::Success);
			success_ = std::move(success);
		}
	}

	void resolveFailure(Failure && failure)
	{
		ASSERT(isPending());

		if (child_) {
			if (child_ != this) {
				child_->opaqueResolveFailure(&failure);
			}
			destruct();
		}
		else {
			setState(PromiseState::Failure);
			failure_ = std::move(failure);
		}
	}

private:
	void destruct()
	{
		if (!isRoot()) {
			delete this;
		}
	}

private:
	Success success_;
	Failure failure_;
};




template <typename Success, typename Failure>
class RootPromise;


template <typename Success, typename Failure>
class Promise {
private:
	template <typename S, typename F>
	friend class Promise;

	template <typename S, typename F>
	friend class RootPromise;

	Promise(Promise const &) = delete;
	void operator= (Promise const &) = delete;

private:
	Promise(PromiseInternals::PromiseImpl<Success, Failure> * impl)
		: impl_(impl)
	{}

	void assign(Promise & other)
	{
		if (other.impl_->isRoot()) {
			root_impl_ = std::move(other.root_impl_);
			impl_ = &root_impl_;
			ASSERT(impl_->isRoot());
		}
		else {
			impl_ = other.impl_;
		}
	}

public:
	Promise(Promise && other)
	{
		assign(other);
	}

	void operator= (Promise && other)
	{
		assign(other);
	}

	void thenTerminate()
	{
		impl_->thenTerminate();
	}

	template <typename ToS, typename ToF>
	auto then(
		ToS && map_success,
		ToF && map_failure)
		-> Promise<decltype(map_success(Success())), decltype(map_failure(Failure()))>
	{
		using S = decltype(map_success(Success()));
		using F = decltype(map_failure(Failure()));
		return Promise<S, F>(impl_->template then<S, F>(std::move(map_success), std::move(map_failure)));
	}

	template <typename ToS>
	auto then(
		ToS && map_success)
		-> Promise<decltype(map_success(Success())), Failure>
	{
		return then(std::move(map_success), PromiseInternals::Move<Failure>());
	}

	template <typename ToF>
	auto catch_(
		ToF && map_failure)
		-> Promise<Success, decltype(map_failure(Failure()))>
	{
		return then(PromiseInternals::Move<Success>(), std::move(map_failure));
	}

private:
	void resolveSuccess(Success && success)
	{
		impl_->resolveSuccess(std::move(success));
	}

	void resolveFailure(Failure && failure)
	{
		impl_->resolveFailure(std::move(failure));
	}

private:
	PromiseInternals::PromiseImpl<Success, Failure> * impl_;
	PromiseInternals::PromiseImpl<Success, Failure> root_impl_;
};


template <typename Success, typename Failure>
class RootPromise : public Promise<Success, Failure> {
private:
	using BasePromise = Promise<Success, Failure>;

public:
	RootPromise()
		: Promise<Success, Failure>(&this->root_impl_) // XXX: Can't do this because pointer may haved changed (think std::vector<RootPromise> resizing)
	{
		this->root_impl_.setIsRoot();
	}

	RootPromise(Success && success)
		: RootPromise()
	{
		resolveSuccess(std::move(success));
	}

	RootPromise(Failure && failure)
		: RootPromise()
	{
		resolveFailure(std::move(failure));
	}

	Promise<Success, Failure> then()
	{
		return Promise<Success, Failure>(this->impl_); // XXX: Can't do this because pointer may haved changed (think std::vector<RootPromise> resizing)
	}

	using BasePromise::resolveSuccess;
	using BasePromise::resolveFailure;
};


