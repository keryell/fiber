
//          Copyright Oliver Kowalke 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_FIBERS_CONTEXT_H
#define BOOST_FIBERS_CONTEXT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#if defined(BOOST_NO_CXX17_STD_APPLY)
#include <boost/context/detail/apply.hpp>
#endif
#include <boost/context/continuation.hpp>
#include <boost/context/stack_context.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/parent_from_member.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/slist.hpp>

#include <boost/fiber/detail/config.hpp>
#include <boost/fiber/detail/decay_copy.hpp>
#include <boost/fiber/detail/fss.hpp>
#include <boost/fiber/detail/spinlock.hpp>
#include <boost/fiber/detail/wrap.hpp>
#include <boost/fiber/exceptions.hpp>
#include <boost/fiber/fixedsize_stack.hpp>
#include <boost/fiber/policy.hpp>
#include <boost/fiber/properties.hpp>
#include <boost/fiber/segmented_stack.hpp>
#include <boost/fiber/type.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable:4251)
#endif

namespace boost {
namespace fibers {

class context;
class fiber;
class scheduler;

namespace detail {

struct wait_tag;
typedef intrusive::slist_member_hook<
    intrusive::tag< wait_tag >,
    intrusive::link_mode<
        intrusive::safe_link
    >
>                                 wait_hook;
// declaration of the functor that converts between
// the context class and the wait-hook
struct wait_functor {
    // required types
    typedef wait_hook               hook_type;
    typedef hook_type           *   hook_ptr;
    typedef const hook_type     *   const_hook_ptr;
    typedef context                 value_type;
    typedef value_type          *   pointer;
    typedef const value_type    *   const_pointer;

    // required static functions
    static hook_ptr to_hook_ptr( value_type &value);
    static const_hook_ptr to_hook_ptr( value_type const& value);
    static pointer to_value_ptr( hook_ptr n);
    static const_pointer to_value_ptr( const_hook_ptr n);
};

struct ready_tag;
typedef intrusive::list_member_hook<
    intrusive::tag< ready_tag >,
    intrusive::link_mode<
        intrusive::auto_unlink
    >
>                                       ready_hook;

struct sleep_tag;
typedef intrusive::set_member_hook<
    intrusive::tag< sleep_tag >,
    intrusive::link_mode<
        intrusive::auto_unlink
    >
>                                       sleep_hook;

struct worker_tag;
typedef intrusive::list_member_hook<
    intrusive::tag< worker_tag >,
    intrusive::link_mode<
        intrusive::auto_unlink
    >
>                                       worker_hook;

struct terminated_tag;
typedef intrusive::slist_member_hook<
    intrusive::tag< terminated_tag >,
    intrusive::link_mode<
        intrusive::safe_link
    >
>                                       terminated_hook;

struct remote_ready_tag;
typedef intrusive::slist_member_hook<
    intrusive::tag< remote_ready_tag >,
    intrusive::link_mode<
        intrusive::safe_link
    >
>                                       remote_ready_hook;

}

struct main_context_t {};
const main_context_t main_context{};

struct dispatcher_context_t {};
const dispatcher_context_t dispatcher_context{};

struct worker_context_t {};
const worker_context_t worker_context{};

class BOOST_FIBERS_DECL context {
public:
    typedef intrusive::slist<
                context,
                intrusive::function_hook< detail::wait_functor >,
                intrusive::linear< true >,
                intrusive::cache_last< true > 
            >   wait_queue_t;

private:
    friend class scheduler;

    struct fss_data {
        void                                *   vp{ nullptr };
        detail::fss_cleanup_function::ptr_t     cleanup_function{};

        fss_data() noexcept {
        }

        fss_data( void * vp_,
                  detail::fss_cleanup_function::ptr_t const& fn) noexcept :
            vp( vp_),
            cleanup_function( fn) {
            BOOST_ASSERT( cleanup_function);
        }

        void do_cleanup() {
            ( * cleanup_function)( vp);
        }
    };

    typedef std::map< uintptr_t, fss_data >             fss_data_t;

#if ! defined(BOOST_FIBERS_NO_ATOMICS)
    std::atomic< std::size_t >                          use_count_{ 0 };
#else
    std::size_t                                         use_count_{ 0 };
#endif
#if ! defined(BOOST_FIBERS_NO_ATOMICS)
    detail::remote_ready_hook                           remote_ready_hook_{};
#endif
    detail::spinlock                                    splk_{};
    bool                                                terminated_{ false };
    wait_queue_t                                        wait_queue_{};
public:
    detail::wait_hook                                   wait_hook_{};
private:
    scheduler                                       *   scheduler_{ nullptr };
    fss_data_t                                          fss_data_{};
    detail::sleep_hook                                  sleep_hook_{};
    detail::ready_hook                                  ready_hook_{};
    detail::terminated_hook                             terminated_hook_{};
    detail::worker_hook                                 worker_hook_{};
    boost::context::continuation                        c_;
    fiber_properties                                *   properties_{ nullptr };
    std::chrono::steady_clock::time_point               tp_{ (std::chrono::steady_clock::time_point::max)() };
    type                                                type_;
    launch                                              policy_;
    context                                         *   from_ctx_{ nullptr };
    context                                         *   ready_ctx_{ nullptr };
    detail::spinlock_lock                           *   lk_{ nullptr };

    void resume_() noexcept;
    void schedule_( context *) noexcept;

    template< typename Fn, typename Tpl >
    boost::context::continuation
    run_( boost::context::continuation && c, Fn && fn_, Tpl && tpl_) noexcept {
        {
            // fn and tpl must be destroyed before calling terminate()
            typename std::decay< Fn >::type fn = std::forward< Fn >( fn_);
            typename std::decay< Tpl >::type tpl = std::forward< Tpl >( tpl_);
            c = c.resume();
            context * active_ctx = active();
            BOOST_ASSERT( nullptr != active_ctx);
            BOOST_ASSERT( nullptr != active_ctx->from_ctx_);
            active_ctx->from_ctx_->c_ = std::move( c);
            active_ctx->from_ctx_ = nullptr;
            if ( nullptr != active_ctx->lk_) {
                active_ctx->lk_->unlock();
                active_ctx->lk_ = nullptr;
            }
            if ( nullptr != active_ctx->ready_ctx_) {
                active_ctx->schedule_( active_ctx->ready_ctx_);
                active_ctx->ready_ctx_ = nullptr;
            }
#if defined(BOOST_NO_CXX17_STD_APPLY)
           boost::context::detail::apply( std::move( fn), std::move( tpl) );
#else
           std::apply( std::move( fn), std::move( tpl) );
#endif
        }
        // terminate context
        return terminate();
    }

public:
    class id {
    private:
        context  *   impl_{ nullptr };

    public:
        id() = default;

        explicit id( context * impl) noexcept :
            impl_{ impl } {
        }

        bool operator==( id const& other) const noexcept {
            return impl_ == other.impl_;
        }

        bool operator!=( id const& other) const noexcept {
            return impl_ != other.impl_;
        }

        bool operator<( id const& other) const noexcept {
            return impl_ < other.impl_;
        }

        bool operator>( id const& other) const noexcept {
            return other.impl_ < impl_;
        }

        bool operator<=( id const& other) const noexcept {
            return ! ( * this > other);
        }

        bool operator>=( id const& other) const noexcept {
            return ! ( * this < other);
        }

        template< typename charT, class traitsT >
        friend std::basic_ostream< charT, traitsT > &
        operator<<( std::basic_ostream< charT, traitsT > & os, id const& other) {
            if ( nullptr != other.impl_) {
                return os << other.impl_;
            } else {
                return os << "{not-valid}";
            }
        }

        explicit operator bool() const noexcept {
            return nullptr != impl_;
        }

        bool operator!() const noexcept {
            return nullptr == impl_;
        }
    };

    static context * active() noexcept;

    static void reset_active() noexcept;

    // main fiber context
    explicit context( main_context_t) noexcept;

    // dispatcher fiber context
    context( dispatcher_context_t, boost::context::preallocated const&,
             default_stack const&, scheduler *);

    // worker fiber context
    template< typename StackAlloc,
              typename Fn,
              typename Tpl
    >
    context( worker_context_t,
             launch policy,
             boost::context::preallocated palloc, StackAlloc salloc,
             Fn && fn, Tpl && tpl) :
        use_count_{ 1 }, // fiber instance or scheduler owner
        c_{},
        type_{ type::worker_context },
        policy_{ policy } {
# if defined(BOOST_NO_CXX14_GENERIC_LAMBDAS)
            c_ = boost::context::callcc(
                    std::allocator_arg, palloc, salloc,
                      detail::wrap(
                          [this]( typename std::decay< Fn >::type & fn, typename std::decay< Tpl >::type & tpl,
                                  boost::context::continuation && c) mutable noexcept {
                                return run_( std::forward< boost::context::continuation >( c), std::move( fn), std::move( tpl) );
                          },
                          std::forward< Fn >( fn),
                          std::forward< Tpl >( tpl) ) );
# else
            c_ = boost::context::callcc(
                    std::allocator_arg, palloc, salloc,
                    [this,fn=detail::decay_copy( std::forward< Fn >( fn) ),tpl=std::forward< Tpl >( tpl)]
                    (boost::context::continuation && c) mutable noexcept {
                          return run_( std::forward< boost::context::continuation >( c), std::move( fn), std::move( tpl) );
                    });
# endif
    }

    context( context const&) = delete;
    context & operator=( context const&) = delete;

    friend bool
    operator==( context const& lhs, context const& rhs) noexcept {
        return & lhs == & rhs;
    }

    virtual ~context();

    scheduler * get_scheduler() const noexcept {
        return scheduler_;
    }

    id get_id() const noexcept;

    bool is_resumable() const noexcept {
        if ( c_) return true;
        else return false;
    }

    void resume() noexcept;
    void resume( detail::spinlock_lock *) noexcept;
    void resume( context *) noexcept;

    void suspend() noexcept;
    void suspend( detail::spinlock_lock *) noexcept;

    boost::context::continuation suspend_with_cc() noexcept;
    boost::context::continuation terminate() noexcept;

    void join();

    void yield() noexcept;

    bool wait_until( std::chrono::steady_clock::time_point const&) noexcept;
    bool wait_until( std::chrono::steady_clock::time_point const&,
                     detail::spinlock_lock *) noexcept;

    void schedule( context *) noexcept;

    bool is_context( type t) const noexcept {
        return type::none != ( type_ & t);
    }

    void * get_fss_data( void const * vp) const;

    void set_fss_data(
        void const * vp,
        detail::fss_cleanup_function::ptr_t const& cleanup_fn,
        void * data,
        bool cleanup_existing);

    void set_properties( fiber_properties * props) noexcept;

    fiber_properties * get_properties() const noexcept {
        return properties_;
    }

    launch get_policy() const noexcept {
        return policy_;
    }

    bool worker_is_linked() const noexcept;

    bool ready_is_linked() const noexcept;

    bool remote_ready_is_linked() const noexcept;

    bool sleep_is_linked() const noexcept;

    bool terminated_is_linked() const noexcept;

    bool wait_is_linked() const noexcept;

    template< typename List >
    void worker_link( List & lst) noexcept {
        static_assert( std::is_same< typename List::value_traits::hook_type, detail::worker_hook >::value, "not a worker-queue");
        BOOST_ASSERT( ! worker_is_linked() );
        lst.push_back( * this);
    }

    template< typename List >
    void ready_link( List & lst) noexcept {
        static_assert( std::is_same< typename List::value_traits::hook_type, detail::ready_hook >::value, "not a ready-queue");
        BOOST_ASSERT( ! ready_is_linked() );
        lst.push_back( * this);
    }

    template< typename List >
    void remote_ready_link( List & lst) noexcept {
        static_assert( std::is_same< typename List::value_traits::hook_type, detail::remote_ready_hook >::value, "not a remote-ready-queue");
        BOOST_ASSERT( ! remote_ready_is_linked() );
        lst.push_back( * this);
    }

    template< typename Set >
    void sleep_link( Set & set) noexcept {
        static_assert( std::is_same< typename Set::value_traits::hook_type,detail::sleep_hook >::value, "not a sleep-queue");
        BOOST_ASSERT( ! sleep_is_linked() );
        set.insert( * this);
    }

    template< typename List >
    void terminated_link( List & lst) noexcept {
        static_assert( std::is_same< typename List::value_traits::hook_type, detail::terminated_hook >::value, "not a terminated-queue");
        BOOST_ASSERT( ! terminated_is_linked() );
        lst.push_back( * this);
    }

    template< typename List >
    void wait_link( List & lst) noexcept {
        static_assert( std::is_same< typename List::value_traits::hook_type, detail::wait_hook >::value, "not a wait-queue");
        BOOST_ASSERT( ! wait_is_linked() );
        lst.push_back( * this);
    }

    void worker_unlink() noexcept;

    void ready_unlink() noexcept;

    void sleep_unlink() noexcept;

    void detach() noexcept;

    void attach( context *) noexcept;

    friend void intrusive_ptr_add_ref( context * ctx) noexcept {
        BOOST_ASSERT( nullptr != ctx);
        ctx->use_count_.fetch_add( 1, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_release( context * ctx) noexcept {
        BOOST_ASSERT( nullptr != ctx);
        if ( 1 == ctx->use_count_.fetch_sub( 1, std::memory_order_release) ) {
            std::atomic_thread_fence( std::memory_order_acquire);
            boost::context::continuation c = std::move( ctx->c_);
            // destruct context
            ctx->~context();
            // deallocated stack
            c.resume( nullptr);
        }
    }
};

inline
bool operator<( context const& l, context const& r) noexcept {
    return l.get_id() < r.get_id();
}

template< typename StackAlloc, typename Fn, typename ... Args >
static intrusive_ptr< context > make_worker_context( launch policy,
                                                     StackAlloc salloc,
                                                     Fn && fn, Args && ... args) {
    auto sctx = salloc.allocate();
    BOOST_ASSERT( ( sizeof( context) + 2048) < sctx.size); // stack at least of 2kB
	const std::size_t offset = sizeof( context) + 63; 
    // reserve space for control structure
    void * storage = reinterpret_cast< void * >(
            ( reinterpret_cast< uintptr_t >( sctx.sp) - static_cast< uintptr_t >( offset) )
            & ~ static_cast< uintptr_t >( 0xff) );
    void * stack_bottom = reinterpret_cast< void * >(
            reinterpret_cast< uintptr_t >( sctx.sp) - static_cast< uintptr_t >( sctx.size) );
    const std::size_t size = reinterpret_cast< uintptr_t >( storage) - reinterpret_cast< uintptr_t >( stack_bottom);
    // placement new of context on top of fiber's stack
    return intrusive_ptr< context >{ 
            new ( storage) context{
                worker_context,
                policy,
                boost::context::preallocated{ storage, size, sctx },
                salloc,
                std::forward< Fn >( fn),
                std::make_tuple( std::forward< Args >( args) ... ) } };
}

namespace detail {

inline
wait_functor::hook_ptr wait_functor::to_hook_ptr( wait_functor::value_type & value) {
    return & value.wait_hook_;
}

inline
wait_functor::const_hook_ptr wait_functor::to_hook_ptr( wait_functor::value_type const& value) {
    return & value.wait_hook_;
}

inline
wait_functor::pointer wait_functor::to_value_ptr( wait_functor::hook_ptr n) {
    return intrusive::get_parent_from_member< context >( n, & context::wait_hook_);
}

inline
wait_functor::const_pointer wait_functor::to_value_ptr( wait_functor::const_hook_ptr n) {
    return intrusive::get_parent_from_member< context >( n, & context::wait_hook_);
}

}}}

#ifdef _MSC_VER
# pragma warning(pop)
#endif

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_FIBERS_CONTEXT_H
