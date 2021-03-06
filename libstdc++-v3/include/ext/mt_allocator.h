// MT-optimized allocator -*- C++ -*-

// Copyright (C) 2003, 2004 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING.  If not, write to the Free
// Software Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307,
// USA.

// As a special exception, you may use this file as part of a free software
// library without restriction.  Specifically, if other files instantiate
// templates or use macros or inline functions from this file, or you compile
// this file and link it with other files to produce an executable, this
// file does not by itself cause the resulting executable to be covered by
// the GNU General Public License.  This exception does not however
// invalidate any other reasons why the executable file might be covered by
// the GNU General Public License.

/** @file ext/mt_allocator.h
 *  This file is a GNU extension to the Standard C++ Library.
 *  You should only include this header if you are using GCC 3 or later.
 */

#ifndef _MT_ALLOCATOR_H
#define _MT_ALLOCATOR_H 1

#include <new>
#include <cstdlib>
#include <bits/functexcept.h>
#include <bits/gthr.h>
#include <bits/atomicity.h>

namespace __gnu_cxx
{
  /**
   *  This is a fixed size (power of 2) allocator which - when
   *  compiled with thread support - will maintain one freelist per
   *  size per thread plus a "global" one. Steps are taken to limit
   *  the per thread freelist sizes (by returning excess back to
   *  "global").
   *
   *  Further details:
   *  http://gcc.gnu.org/onlinedocs/libstdc++/ext/mt_allocator.html
   */
  typedef void (*__destroy_handler)(void*);
  typedef void (*__create_handler)(void);

  class __pool_base
  {
  public:
    // Variables used to configure the behavior of the allocator,
    // assigned and explained in detail below.
    struct _Tune
    {
      // Alignment needed.
      // NB: In any case must be >= sizeof(_Block_record), that
      // is 4 on 32 bit machines and 8 on 64 bit machines.
      size_t  _M_align;
      
      // Allocation requests (after round-up to power of 2) below
      // this value will be handled by the allocator. A raw new/
      // call will be used for requests larger than this value.
      size_t	_M_max_bytes; 
      
      // Size in bytes of the smallest bin.
      // NB: Must be a power of 2 and >= _M_align.
      size_t  _M_min_bin;
      
      // In order to avoid fragmenting and minimize the number of
      // new() calls we always request new memory using this
      // value. Based on previous discussions on the libstdc++
      // mailing list we have choosen the value below.
      // See http://gcc.gnu.org/ml/libstdc++/2001-07/msg00077.html
      size_t 	_M_chunk_size;
      
      // The maximum number of supported threads. For
      // single-threaded operation, use one. Maximum values will
      // vary depending on details of the underlying system. (For
      // instance, Linux 2.4.18 reports 4070 in
      // /proc/sys/kernel/threads-max, while Linux 2.6.6 reports
      // 65534)
      size_t 	_M_max_threads;
      
      // Each time a deallocation occurs in a threaded application
      // we make sure that there are no more than
      // _M_freelist_headroom % of used memory on the freelist. If
      // the number of additional records is more than
      // _M_freelist_headroom % of the freelist, we move these
      // records back to the global pool.
      size_t 	_M_freelist_headroom;
      
      // Set to true forces all allocations to use new().
      bool 	_M_force_new; 
      
      explicit
      _Tune()
      : _M_align(8), _M_max_bytes(128), _M_min_bin(8),
      _M_chunk_size(4096 - 4 * sizeof(void*)), 
      _M_max_threads(4096), _M_freelist_headroom(10), 
      _M_force_new(getenv("GLIBCXX_FORCE_NEW") ? true : false)
      { }

      explicit
      _Tune(size_t __align, size_t __maxb, size_t __minbin, size_t __chunk, 
	    size_t __maxthreads, size_t __headroom, bool __force) 
      : _M_align(__align), _M_max_bytes(__maxb), _M_min_bin(__minbin),
      _M_chunk_size(__chunk), _M_max_threads(__maxthreads),
      _M_freelist_headroom(__headroom), _M_force_new(__force)
      { }
    };
    
    const _Tune&
    _M_get_options() const
    { return _M_options; }

    void
    _M_set_options(_Tune __t)
    { 
      if (!_M_init)
	_M_options = __t;
    }

    bool
    _M_check_threshold(size_t __bytes)
    { return __bytes > _M_options._M_max_bytes || _M_options._M_force_new; }

    size_t
    _M_get_binmap(size_t __bytes)
    { return _M_binmap[__bytes]; }

    explicit __pool_base() 
    : _M_init(false), _M_options(_Tune()), _M_binmap(NULL) { }

  protected:
    // We need to create the initial lists and set up some variables
    // before we can answer to the first request for memory.
    bool 			_M_init;
    
    // Configuration options.
    _Tune 	       		_M_options;
    
    // Using short int as type for the binmap implies we are never
    // caching blocks larger than 65535 with this allocator.
    typedef unsigned short int  _Binmap_type;
    _Binmap_type* 		_M_binmap;
  };

  // Data describing the underlying memory pool, parameterized on
  // threading support.
  template<bool _Thread>
    class __pool;

  template<>
    class __pool<true>;

  template<>
    class __pool<false>;


#ifdef __GTHREADS
  // Specialization for thread enabled, via gthreads.h.
  template<>
    class __pool<true> : public __pool_base
    {
    public:
      // Each requesting thread is assigned an id ranging from 1 to
      // _S_max_threads. Thread id 0 is used as a global memory pool.
      // In order to get constant performance on the thread assignment
      // routine, we keep a list of free ids. When a thread first
      // requests memory we remove the first record in this list and
      // stores the address in a __gthread_key. When initializing the
      // __gthread_key we specify a destructor. When this destructor
      // (i.e. the thread dies) is called, we return the thread id to
      // the front of this list.
      struct _Thread_record
      {
	// Points to next free thread id record. NULL if last record in list.
	_Thread_record* volatile        _M_next;
	
	// Thread id ranging from 1 to _S_max_threads.
	size_t                          _M_id;
      };
      
      union _Block_record
      {
	// Points to the block_record of the next free block.
	_Block_record* volatile         _M_next;
	
	// The thread id of the thread which has requested this block.
	size_t                          _M_thread_id;
      };
      
      struct _Bin_record
      {
	// An "array" of pointers to the first free block for each
	// thread id. Memory to this "array" is allocated in _S_initialize()
	// for _S_max_threads + global pool 0.
	_Block_record** volatile        _M_first;
	
	// An "array" of counters used to keep track of the amount of
	// blocks that are on the freelist/used for each thread id.
	// Memory to these "arrays" is allocated in _S_initialize() for
	// _S_max_threads + global pool 0.
	size_t* volatile                _M_free;
	size_t* volatile                _M_used;
	
	// Each bin has its own mutex which is used to ensure data
	// integrity while changing "ownership" on a block.  The mutex
	// is initialized in _S_initialize().
	__gthread_mutex_t*              _M_mutex;
      };
      
      void
      _M_initialize(__destroy_handler __d);

      void
      _M_initialize_once(__create_handler __c)
      {
	// Although the test in __gthread_once() would suffice, we
	// wrap test of the once condition in our own unlocked
	// check. This saves one function call to pthread_once()
	// (which itself only tests for the once value unlocked anyway
	// and immediately returns if set)
	if (__builtin_expect(_M_init == false, false))
	  {
	    if (__gthread_active_p())
	      __gthread_once(&_M_once, __c);
	    if (!_M_init)
	      __c();
	  }
      }

      char* 
      _M_reserve_memory(size_t __bytes, const size_t __thread_id);
    
      void
      _M_reclaim_memory(char* __p, size_t __bytes);
    
      const _Bin_record&
      _M_get_bin(size_t __which)
      { return _M_bin[__which]; }
      
      void
      _M_adjust_freelist(const _Bin_record& __bin, _Block_record* __block, 
			 size_t __thread_id)
      {
	if (__gthread_active_p())
	  {
	    __block->_M_thread_id = __thread_id;
	    --__bin._M_free[__thread_id];
	    ++__bin._M_used[__thread_id];
	  }
      }

      void 
      _M_destroy_thread_key(void* __freelist_pos);

      size_t 
      _M_get_thread_id();

      explicit __pool() 
      : _M_bin(NULL), _M_bin_size(1), _M_thread_freelist(NULL) 
      {
	// On some platforms, __gthread_once_t is an aggregate.
	__gthread_once_t __tmp = __GTHREAD_ONCE_INIT;
	_M_once = __tmp;
      }

    private:
      // An "array" of bin_records each of which represents a specific
      // power of 2 size. Memory to this "array" is allocated in
      // _M_initialize().
      _Bin_record* volatile	_M_bin;

      // Actual value calculated in _M_initialize().
      size_t 	       	     	_M_bin_size;

      __gthread_once_t 		_M_once;
      
      _Thread_record* 		_M_thread_freelist;
    };
#endif

  // Specialization for single thread.
  template<>
    class __pool<false> : public __pool_base
    {
    public:
      union _Block_record
      {
	// Points to the block_record of the next free block.
	_Block_record* volatile         _M_next;
      };
      
      struct _Bin_record
      {
	// An "array" of pointers to the first free block for each
	// thread id. Memory to this "array" is allocated in _S_initialize()
	// for _S_max_threads + global pool 0.
	_Block_record** volatile        _M_first;
      };
      
      void
      _M_initialize_once()
      {
	if (__builtin_expect(_M_init == false, false))
	  _M_initialize();
      }

      char* 
      _M_reserve_memory(size_t __bytes, const size_t __thread_id);
    
      void
      _M_reclaim_memory(char* __p, size_t __bytes);
    
      size_t 
      _M_get_thread_id() { return 0; }
      
      const _Bin_record&
      _M_get_bin(size_t __which)
      { return _M_bin[__which]; }
      
      void
      _M_adjust_freelist(const _Bin_record&, _Block_record*, size_t)
      { }

      explicit __pool() 
      : _M_bin(NULL), _M_bin_size(1) { }
      
    private:
      // An "array" of bin_records each of which represents a specific
      // power of 2 size. Memory to this "array" is allocated in
      // _M_initialize().
      _Bin_record* volatile	_M_bin;
      
      // Actual value calculated in _M_initialize().
      size_t 	       	     	_M_bin_size;     

      void
      _M_initialize();
  };


  template<bool _Thread>
    struct __common_pool_policy 
    {
      template<typename _Tp1, bool _Thread1 = _Thread>
        struct _M_rebind;

      template<typename _Tp1>
        struct _M_rebind<_Tp1, true>
        { typedef __common_pool_policy<true> other; };

      template<typename _Tp1>
        struct _M_rebind<_Tp1, false>
        { typedef __common_pool_policy<false> other; };

      typedef __pool<_Thread> __pool_type;
      static __pool_type	_S_data;

      static __pool_type&
      _S_get_pool();

      static void
      _S_initialize_once() 
      { 
	static bool __init;
	if (__builtin_expect(__init == false, false))
	  {
	    _S_get_pool()._M_initialize_once(); 
	    __init = true;
	  }
      }
    };

  template<>
    struct __common_pool_policy<true>;

#ifdef __GTHREADS
  template<>
    struct __common_pool_policy<true>
    {
      template<typename _Tp1, bool _Thread1 = true>
        struct _M_rebind;

      template<typename _Tp1>
        struct _M_rebind<_Tp1, true>
        { typedef __common_pool_policy<true> other; };

      template<typename _Tp1>
        struct _M_rebind<_Tp1, false>
        { typedef __common_pool_policy<false> other; };

      typedef __pool<true> __pool_type;
      static __pool_type	_S_data;

      static __pool_type&
      _S_get_pool();

      static void
      _S_destroy_thread_key(void* __freelist_pos)
      { _S_get_pool()._M_destroy_thread_key(__freelist_pos); }
      
      static void
      _S_initialize() 
      { _S_get_pool()._M_initialize(_S_destroy_thread_key); }

      static void
      _S_initialize_once() 
      { 
	static bool __init;
	if (__builtin_expect(__init == false, false))
	  {
	    _S_get_pool()._M_initialize_once(_S_initialize); 
	    __init = true;
	  }
      }
   };
#endif

  template<typename _Tp, bool _Thread>
    struct __per_type_pool_policy
    {
      template<typename _Tp1, bool _Thread1 = _Thread>
        struct _M_rebind;

      template<typename _Tp1>
        struct _M_rebind<_Tp1, false>
        { typedef __per_type_pool_policy<_Tp1, false> other; };

      template<typename _Tp1>
        struct _M_rebind<_Tp1, true>
        { typedef __per_type_pool_policy<_Tp1, true> other; };

      typedef __pool<_Thread> __pool_type;
      static __pool_type	_S_data;

      static __pool_type&
      _S_get_pool( ) { return _S_data; }

      static void
      _S_initialize_once() 
      { 
	static bool __init;
	if (__builtin_expect(__init == false, false))
	  {
	    _S_get_pool()._M_initialize_once(); 
	    __init = true;
	  }
      }
    };

  template<typename _Tp, bool _Thread>
    __pool<_Thread>
    __per_type_pool_policy<_Tp, _Thread>::_S_data;

  template<typename _Tp>
    struct __per_type_pool_policy<_Tp, true>;

#ifdef __GTHREADS
  template<typename _Tp>
    struct __per_type_pool_policy<_Tp, true>
    {
      template<typename _Tp1, bool _Thread1 = true>
        struct _M_rebind;

      template<typename _Tp1>
        struct _M_rebind<_Tp1, false>
        { typedef __per_type_pool_policy<_Tp1, false> other; };

      template<typename _Tp1>
        struct _M_rebind<_Tp1, true>
        { typedef __per_type_pool_policy<_Tp1, true> other; };

      typedef __pool<true> __pool_type;
      static __pool_type	_S_data;

      static __pool_type&
      _S_get_pool( ) { return _S_data; }

      static void
      _S_destroy_thread_key(void* __freelist_pos)
      { _S_get_pool()._M_destroy_thread_key(__freelist_pos); }
      
      static void
      _S_initialize() 
      { _S_get_pool()._M_initialize(_S_destroy_thread_key); }

      static void
      _S_initialize_once() 
      { 
	static bool __init;
	if (__builtin_expect(__init == false, false))
	  {
	    _S_get_pool()._M_initialize_once(_S_initialize); 
	    __init = true;
	  }
      }
    };

  template<typename _Tp>
    __pool<true>
    __per_type_pool_policy<_Tp, true>::_S_data;
#endif

#ifdef __GTHREADS
  typedef __common_pool_policy<true> __default_policy;
#else
  typedef __common_pool_policy<false> __default_policy;
#endif

  template<typename _Tp>
    class __mt_alloc_base 
    {
    public:
      typedef size_t                    size_type;
      typedef ptrdiff_t                 difference_type;
      typedef _Tp*                      pointer;
      typedef const _Tp*                const_pointer;
      typedef _Tp&                      reference;
      typedef const _Tp&                const_reference;
      typedef _Tp                       value_type;

      pointer
      address(reference __x) const
      { return &__x; }

      const_pointer
      address(const_reference __x) const
      { return &__x; }

      size_type
      max_size() const throw() 
      { return size_t(-1) / sizeof(_Tp); }

      // _GLIBCXX_RESOLVE_LIB_DEFECTS
      // 402. wrong new expression in [some_] allocator::construct
      void 
      construct(pointer __p, const _Tp& __val) 
      { ::new(__p) _Tp(__val); }

      void 
      destroy(pointer __p) { __p->~_Tp(); }
    };

  template<typename _Tp, typename _Poolp = __default_policy>
    class __mt_alloc : public __mt_alloc_base<_Tp>,  _Poolp
    {
    public:
      typedef size_t                    size_type;
      typedef ptrdiff_t                 difference_type;
      typedef _Tp*                      pointer;
      typedef const _Tp*                const_pointer;
      typedef _Tp&                      reference;
      typedef const _Tp&                const_reference;
      typedef _Tp                       value_type;
      typedef _Poolp                  	__policy_type;
      typedef typename _Poolp::__pool_type       __pool_type;

      template<typename _Tp1, typename _Poolp1 = _Poolp>
        struct rebind
        { 
	  typedef typename _Poolp1::template _M_rebind<_Tp1>::other pol_type;
	  typedef __mt_alloc<_Tp1, pol_type> other;
	};

      __mt_alloc() throw() 
      {
	// XXX
      }

      __mt_alloc(const __mt_alloc&) throw() 
      {
	// XXX
      }

      template<typename _Tp1, typename _Poolp1>
        __mt_alloc(const __mt_alloc<_Tp1, _Poolp1>& obj) throw()  
        {
	  // XXX
	}

      ~__mt_alloc() throw() { }

      pointer
      allocate(size_type __n, const void* = 0);

      void
      deallocate(pointer __p, size_type __n);

      const __pool_base::_Tune
      _M_get_options()
      { 
	// Return a copy, not a reference, for external consumption.
	return __pool_base::_Tune(this->_S_get_pool()._M_get_options()); 
      }
      
      void
      _M_set_options(__pool_base::_Tune __t)
      { this->_S_get_pool()._M_set_options(__t); }
    };

  template<typename _Tp, typename _Poolp>
    typename __mt_alloc<_Tp, _Poolp>::pointer
    __mt_alloc<_Tp, _Poolp>::
    allocate(size_type __n, const void*)
    {
      this->_S_initialize_once();

      // Requests larger than _M_max_bytes are handled by new/delete
      // directly.
      __pool_type& __pl = this->_S_get_pool();
      const size_t __bytes = __n * sizeof(_Tp);
      if (__pl._M_check_threshold(__bytes))
	{
	  void* __ret = ::operator new(__bytes);
	  return static_cast<_Tp*>(__ret);
	}

      // Round up to power of 2 and figure out which bin to use.
      const size_t __which = __pl._M_get_binmap(__bytes);
      const size_t __thread_id = __pl._M_get_thread_id();
      
      // Find out if we have blocks on our freelist.  If so, go ahead
      // and use them directly without having to lock anything.
      char* __c;
      typedef typename __pool_type::_Bin_record _Bin_record;
      const _Bin_record& __bin = __pl._M_get_bin(__which);
      if (__bin._M_first[__thread_id])
	{
	  // Already reserved.
	  typedef typename __pool_type::_Block_record _Block_record;
	  _Block_record* __block = __bin._M_first[__thread_id];
	  __bin._M_first[__thread_id] = __bin._M_first[__thread_id]->_M_next;
	  
	  __pl._M_adjust_freelist(__bin, __block, __thread_id);
	  const __pool_base::_Tune& __options = __pl._M_get_options();
	  __c = reinterpret_cast<char*>(__block) + __options._M_align;
	}
      else
	{
	  // Null, reserve.
	  __c = __pl._M_reserve_memory(__bytes, __thread_id);
	}
      return static_cast<_Tp*>(static_cast<void*>(__c));
    }
  
  template<typename _Tp, typename _Poolp>
    void
    __mt_alloc<_Tp, _Poolp>::
    deallocate(pointer __p, size_type __n)
    {
      // Requests larger than _M_max_bytes are handled by operators
      // new/delete directly.
      __pool_type& __pl = this->_S_get_pool();
      const size_t __bytes = __n * sizeof(_Tp);
      if (__pl._M_check_threshold(__bytes))
	::operator delete(__p);
      else
	__pl._M_reclaim_memory(reinterpret_cast<char*>(__p), __bytes);
    }
  
  template<typename _Tp, typename _Poolp>
    inline bool
    operator==(const __mt_alloc<_Tp, _Poolp>&, const __mt_alloc<_Tp, _Poolp>&)
    { return true; }
  
  template<typename _Tp, typename _Poolp>
    inline bool
    operator!=(const __mt_alloc<_Tp, _Poolp>&, const __mt_alloc<_Tp, _Poolp>&)
    { return false; }
} // namespace __gnu_cxx

#endif
