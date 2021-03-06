// Allocator details.

// Copyright (C) 2004 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Librarbooly.  This library is free
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

//
// ISO C++ 14882:
//

#include <bits/c++config.h>
#include <ext/mt_allocator.h>
#include <bits/concurrence.h>

namespace __gnu_internal
{
  __glibcxx_mutex_define_initialized(freelist_mutex);

#ifdef __GTHREADS
  __gthread_key_t freelist_key;
#endif
}

namespace __gnu_cxx
{
#ifdef __GTHREADS
  void
  __pool<true>::_M_reclaim_memory(char* __p, size_t __bytes)
  {
    // Round up to power of 2 and figure out which bin to use.
    const size_t __which = _M_binmap[__bytes];
    const _Bin_record& __bin = _M_bin[__which];
    const _Tune& __options = _M_get_options();
    
    char* __c = __p - __options._M_align;
    _Block_record* __block = reinterpret_cast<_Block_record*>(__c);
    
    if (__gthread_active_p())
      {
	// Calculate the number of records to remove from our freelist:
	// in order to avoid too much contention we wait until the
	// number of records is "high enough".
	const size_t __thread_id = _M_get_thread_id();
	
	long __remove = ((__bin._M_free[__thread_id] 
			  * __options._M_freelist_headroom)
			 - __bin._M_used[__thread_id]);
	if (__remove > static_cast<long>(100 * (_M_bin_size - __which)
					 * __options._M_freelist_headroom)
	    && __remove > static_cast<long>(__bin._M_free[__thread_id]))
	  {
	    _Block_record* __tmp = __bin._M_first[__thread_id];
	    _Block_record* __first = __tmp;
	    __remove /= __options._M_freelist_headroom;
	    const long __removed = __remove;
	    --__remove;
	    while (__remove-- > 0)
	      __tmp = __tmp->_M_next;
	    __bin._M_first[__thread_id] = __tmp->_M_next;
	    __bin._M_free[__thread_id] -= __removed;
	    
	    __gthread_mutex_lock(__bin._M_mutex);
	    __tmp->_M_next = __bin._M_first[0];
	    __bin._M_first[0] = __first;
	    __bin._M_free[0] += __removed;
	    __gthread_mutex_unlock(__bin._M_mutex);
	  }
	
	// Return this block to our list and update counters and
	// owner id as needed.
	--__bin._M_used[__block->_M_thread_id];
	
	__block->_M_next = __bin._M_first[__thread_id];
	__bin._M_first[__thread_id] = __block;
	
	++__bin._M_free[__thread_id];
      }
    else
      {
	// Not using threads, so single threaded application - return
	// to global pool.
	__block->_M_next = __bin._M_first[0];
	__bin._M_first[0] = __block;
      }
  }
#endif

  void
  __pool<false>::_M_reclaim_memory(char* __p, size_t __bytes)
  {
    // Round up to power of 2 and figure out which bin to use.
    const size_t __which = _M_binmap[__bytes];
    const _Bin_record& __bin = _M_bin[__which];
    const _Tune& __options = _M_get_options();
      
    char* __c = __p - __options._M_align;
    _Block_record* __block = reinterpret_cast<_Block_record*>(__c);
      
    // Single threaded application - return to global pool.
    __block->_M_next = __bin._M_first[0];
    __bin._M_first[0] = __block;
  }

#ifdef __GTHREADS
  char* 
  __pool<true>::_M_reserve_memory(size_t __bytes, const size_t __thread_id)
  {
    // Round up to power of 2 and figure out which bin to use.
    const size_t __which = _M_binmap[__bytes];
      
    // If here, there are no blocks on our freelist.
    const _Tune& __options = _M_get_options();
    _Block_record* __block = NULL;
    const _Bin_record& __bin = _M_bin[__which];

    // NB: For alignment reasons, we can't use the first _M_align
    // bytes, even when sizeof(_Block_record) < _M_align.
    const size_t __bin_size = ((__options._M_min_bin << __which)
			       + __options._M_align);
    size_t __block_count = __options._M_chunk_size / __bin_size;	  
    
    // Are we using threads?
    // - Yes, check if there are free blocks on the global
    //   list. If so, grab up to __block_count blocks in one
    //   lock and change ownership. If the global list is 
    //   empty, we allocate a new chunk and add those blocks 
    //   directly to our own freelist (with us as owner).
    // - No, all operations are made directly to global pool 0
    //   no need to lock or change ownership but check for free
    //   blocks on global list (and if not add new ones) and
    //   get the first one.
    if (__gthread_active_p())
      {
	__gthread_mutex_lock(__bin._M_mutex);
	if (__bin._M_first[0] == NULL)
	  {
	    // No need to hold the lock when we are adding a
	    // whole chunk to our own list.
	    __gthread_mutex_unlock(__bin._M_mutex);
	    
	    void* __v = ::operator new(__options._M_chunk_size);
	    __bin._M_first[__thread_id] = static_cast<_Block_record*>(__v);
	    __bin._M_free[__thread_id] = __block_count;
	    
	    --__block_count;
	    __block = __bin._M_first[__thread_id];
	    while (__block_count-- > 0)
	      {
		char* __c = reinterpret_cast<char*>(__block) + __bin_size;
		__block->_M_next = reinterpret_cast<_Block_record*>(__c);
		__block = __block->_M_next;
	      }
	    __block->_M_next = NULL;
	  }
	else
	  {
	    // Is the number of required blocks greater than or
	    // equal to the number that can be provided by the
	    // global free list?
	    __bin._M_first[__thread_id] = __bin._M_first[0];
	    if (__block_count >= __bin._M_free[0])
	      {
		__bin._M_free[__thread_id] = __bin._M_free[0];
		__bin._M_free[0] = 0;
		__bin._M_first[0] = NULL;
	      }
	    else
	      {
		__bin._M_free[__thread_id] = __block_count;
		__bin._M_free[0] -= __block_count;
		--__block_count;
		__block = __bin._M_first[0];
		while (__block_count-- > 0)
		  __block = __block->_M_next;
		__bin._M_first[0] = __block->_M_next;
		__block->_M_next = NULL;
	      }
	    __gthread_mutex_unlock(__bin._M_mutex);
	  }
      }
    else
      {
	void* __v = ::operator new(__options._M_chunk_size);
	__bin._M_first[0] = static_cast<_Block_record*>(__v);
	
	--__block_count;
	__block = __bin._M_first[0];
	while (__block_count-- > 0)
	  {
	    char* __c = reinterpret_cast<char*>(__block) + __bin_size;
	    __block->_M_next = reinterpret_cast<_Block_record*>(__c);
	    __block = __block->_M_next;
	  }
	__block->_M_next = NULL;
      }
      
    __block = __bin._M_first[__thread_id];
    __bin._M_first[__thread_id] = __bin._M_first[__thread_id]->_M_next;

    if (__gthread_active_p())
      {
	__block->_M_thread_id = __thread_id;
	--__bin._M_free[__thread_id];
	++__bin._M_used[__thread_id];
      }
    return reinterpret_cast<char*>(__block) + __options._M_align;
  }
#endif

  char* 
  __pool<false>::_M_reserve_memory(size_t __bytes, const size_t __thread_id)
  {
    // Round up to power of 2 and figure out which bin to use.
    const size_t __which = _M_binmap[__bytes];
      
    // If here, there are no blocks on our freelist.
    const _Tune& __options = _M_get_options();
    _Block_record* __block = NULL;
    const _Bin_record& __bin = _M_bin[__which];
    
    // NB: For alignment reasons, we can't use the first _M_align
    // bytes, even when sizeof(_Block_record) < _M_align.
    const size_t __bin_size = ((__options._M_min_bin << __which) 
			       + __options._M_align);
    size_t __block_count = __options._M_chunk_size / __bin_size;	  
	  
    // Not using threads.
    void* __v = ::operator new(__options._M_chunk_size);
    __bin._M_first[0] = static_cast<_Block_record*>(__v);
    
    --__block_count;
    __block = __bin._M_first[0];
    while (__block_count-- > 0)
      {
	char* __c = reinterpret_cast<char*>(__block) + __bin_size;
	__block->_M_next = reinterpret_cast<_Block_record*>(__c);
	__block = __block->_M_next;
      }
    __block->_M_next = NULL;
      
    __block = __bin._M_first[__thread_id];
    __bin._M_first[__thread_id] = __bin._M_first[__thread_id]->_M_next;
    return reinterpret_cast<char*>(__block) + __options._M_align;
  }

#ifdef __GTHREADS
 void
  __pool<true>::_M_initialize(__destroy_handler __d)
  {
    // This method is called on the first allocation (when _M_init
    // is still false) to create the bins.
    
    // _M_force_new must not change after the first allocate(),
    // which in turn calls this method, so if it's false, it's false
    // forever and we don't need to return here ever again.
    if (_M_options._M_force_new) 
      {
	_M_init = true;
	return;
      }
      
    // Calculate the number of bins required based on _M_max_bytes.
    // _M_bin_size is statically-initialized to one.
    size_t __bin_size = _M_options._M_min_bin;
    while (_M_options._M_max_bytes > __bin_size)
      {
	__bin_size <<= 1;
	++_M_bin_size;
      }
      
    // Setup the bin map for quick lookup of the relevant bin.
    const size_t __j = (_M_options._M_max_bytes + 1) * sizeof(_Binmap_type);
    _M_binmap = static_cast<_Binmap_type*>(::operator new(__j));
      
    _Binmap_type* __bp = _M_binmap;
    _Binmap_type __bin_max = _M_options._M_min_bin;
    _Binmap_type __bint = 0;
    for (_Binmap_type __ct = 0; __ct <= _M_options._M_max_bytes; ++__ct)
      {
	if (__ct > __bin_max)
	  {
	    __bin_max <<= 1;
	    ++__bint;
	  }
	*__bp++ = __bint;
      }
      
    // Initialize _M_bin and its members.
    void* __v = ::operator new(sizeof(_Bin_record) * _M_bin_size);
    _M_bin = static_cast<_Bin_record*>(__v);
      
    // If __gthread_active_p() create and initialize the list of
    // free thread ids. Single threaded applications use thread id 0
    // directly and have no need for this.
    if (__gthread_active_p())
      {
	const size_t __k = sizeof(_Thread_record) * _M_options._M_max_threads;
	__v = ::operator new(__k);
	_M_thread_freelist = static_cast<_Thread_record*>(__v);
	  
	// NOTE! The first assignable thread id is 1 since the
	// global pool uses id 0
	size_t __i;
	for (__i = 1; __i < _M_options._M_max_threads; ++__i)
	  {
	    _Thread_record& __tr = _M_thread_freelist[__i - 1];
	    __tr._M_next = &_M_thread_freelist[__i];
	    __tr._M_id = __i;
	  }
	  
	// Set last record.
	_M_thread_freelist[__i - 1]._M_next = NULL;
	_M_thread_freelist[__i - 1]._M_id = __i;
	  
	// Initialize per thread key to hold pointer to
	// _M_thread_freelist.
	__gthread_key_create(&__gnu_internal::freelist_key, __d);
	  
	const size_t __max_threads = _M_options._M_max_threads + 1;
	for (size_t __n = 0; __n < _M_bin_size; ++__n)
	  {
	    _Bin_record& __bin = _M_bin[__n];
	    __v = ::operator new(sizeof(_Block_record*) * __max_threads);
	    __bin._M_first = static_cast<_Block_record**>(__v);
	      
	    __v = ::operator new(sizeof(size_t) * __max_threads);
	    __bin._M_free = static_cast<size_t*>(__v);
	      
	    __v = ::operator new(sizeof(size_t) * __max_threads);
	    __bin._M_used = static_cast<size_t*>(__v);
	      
	    __v = ::operator new(sizeof(__gthread_mutex_t));
	    __bin._M_mutex = static_cast<__gthread_mutex_t*>(__v);
	      
#ifdef __GTHREAD_MUTEX_INIT
	    {
	      // Do not copy a POSIX/gthr mutex once in use.
	      __gthread_mutex_t __tmp = __GTHREAD_MUTEX_INIT;
	      *__bin._M_mutex = __tmp;
	    }
#else
	    { __GTHREAD_MUTEX_INIT_FUNCTION(__bin._M_mutex); }
#endif
	      
	    for (size_t __threadn = 0; __threadn < __max_threads;
		 ++__threadn)
	      {
		__bin._M_first[__threadn] = NULL;
		__bin._M_free[__threadn] = 0;
		__bin._M_used[__threadn] = 0;
	      }
	  }
      }
    else
      for (size_t __n = 0; __n < _M_bin_size; ++__n)
	{
	  _Bin_record& __bin = _M_bin[__n];
	  __v = ::operator new(sizeof(_Block_record*));
	  __bin._M_first = static_cast<_Block_record**>(__v);
	  __bin._M_first[0] = NULL;
	}
    _M_init = true;
  }
#endif

  void
  __pool<false>::_M_initialize()
  {
    // This method is called on the first allocation (when _M_init
    // is still false) to create the bins.
    
    // _M_force_new must not change after the first allocate(),
    // which in turn calls this method, so if it's false, it's false
    // forever and we don't need to return here ever again.
    if (_M_options._M_force_new) 
      {
	_M_init = true;
	return;
      }
      
    // Calculate the number of bins required based on _M_max_bytes.
    // _M_bin_size is statically-initialized to one.
    size_t __bin_size = _M_options._M_min_bin;
    while (_M_options._M_max_bytes > __bin_size)
      {
	__bin_size <<= 1;
	++_M_bin_size;
      }
      
    // Setup the bin map for quick lookup of the relevant bin.
    const size_t __j = (_M_options._M_max_bytes + 1) * sizeof(_Binmap_type);
    _M_binmap = static_cast<_Binmap_type*>(::operator new(__j));
      
    _Binmap_type* __bp = _M_binmap;
    _Binmap_type __bin_max = _M_options._M_min_bin;
    _Binmap_type __bint = 0;
    for (_Binmap_type __ct = 0; __ct <= _M_options._M_max_bytes; ++__ct)
      {
	if (__ct > __bin_max)
	  {
	    __bin_max <<= 1;
	    ++__bint;
	  }
	*__bp++ = __bint;
      }
      
    // Initialize _M_bin and its members.
    void* __v = ::operator new(sizeof(_Bin_record) * _M_bin_size);
    _M_bin = static_cast<_Bin_record*>(__v);
      
    for (size_t __n = 0; __n < _M_bin_size; ++__n)
      {
	_Bin_record& __bin = _M_bin[__n];
	__v = ::operator new(sizeof(_Block_record*));
	__bin._M_first = static_cast<_Block_record**>(__v);
	__bin._M_first[0] = NULL;
      }
    _M_init = true;
  }
  
#ifdef __GTHREADS
  size_t
  __pool<true>::_M_get_thread_id()
  {
    // If we have thread support and it's active we check the thread
    // key value and return its id or if it's not set we take the
    // first record from _M_thread_freelist and sets the key and
    // returns it's id.
    if (__gthread_active_p())
      {
	void* v = __gthread_getspecific(__gnu_internal::freelist_key);
	_Thread_record* __freelist_pos = static_cast<_Thread_record*>(v); 
	if (__freelist_pos == NULL)
	  {
	    // Since _M_options._M_max_threads must be larger than
	    // the theoretical max number of threads of the OS the
	    // list can never be empty.
	    {
	      __gnu_cxx::lock sentry(__gnu_internal::freelist_mutex);
	      __freelist_pos = _M_thread_freelist;
	      _M_thread_freelist = _M_thread_freelist->_M_next;
	    }
	      
	    __gthread_setspecific(__gnu_internal::freelist_key, 
				  static_cast<void*>(__freelist_pos));
	  }
	return __freelist_pos->_M_id;
      }

    // Otherwise (no thread support or inactive) all requests are
    // served from the global pool 0.
    return 0;
  }

  void
  __pool<true>::_M_destroy_thread_key(void* __freelist_pos)
  {
    // Return this thread id record to front of thread_freelist.
    __gnu_cxx::lock sentry(__gnu_internal::freelist_mutex);
    _Thread_record* __tr = static_cast<_Thread_record*>(__freelist_pos);
    __tr->_M_next = _M_thread_freelist; 
    _M_thread_freelist = __tr;
  }
#endif

  // Definitions for non-exported bits of __common_pool.
#ifdef __GTHREADS
  __pool<true>
  __common_pool_policy<true>::_S_data = __pool<true>();

  __pool<true>&
  __common_pool_policy<true>::_S_get_pool() { return _S_data; }
#endif

  template<>
    __pool<false>
    __common_pool_policy<false>::_S_data = __pool<false>();

  template<>
    __pool<false>&
    __common_pool_policy<false>::_S_get_pool() { return _S_data; }

  // Instantiations.
  template class __mt_alloc<char>;
  template class __mt_alloc<wchar_t>;
} // namespace __gnu_cxx
