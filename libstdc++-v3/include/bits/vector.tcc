// Vector implementation (out of line) -*- C++ -*-

// Copyright (C) 2001, 2002, 2003, 2004 Free Software Foundation, Inc.
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

/*
 *
 * Copyright (c) 1994
 * Hewlett-Packard Company
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Hewlett-Packard Company makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 *
 * Copyright (c) 1996
 * Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Silicon Graphics makes no
 * representations about the suitability of this  software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 */

/** @file vector.tcc
 *  This is an internal header file, included by other library headers.
 *  You should not attempt to use it directly.
 */

#ifndef _VECTOR_TCC
#define _VECTOR_TCC 1

namespace _GLIBCXX_STD
{
  template<typename _Tp, typename _Alloc>
    void
    vector<_Tp, _Alloc>::
    reserve(size_type __n)
    {
      if (__n > this->max_size())
	__throw_length_error(__N("vector::reserve"));
      if (this->capacity() < __n)
	{
	  const size_type __old_size = size();
	  pointer __tmp = _M_allocate_and_copy(__n,
					       this->_M_impl._M_start,
					       this->_M_impl._M_finish);
	  std::_Destroy(this->_M_impl._M_start, this->_M_impl._M_finish,
			this->get_allocator());
	  _M_deallocate(this->_M_impl._M_start,
			this->_M_impl._M_end_of_storage
			- this->_M_impl._M_start);
	  this->_M_impl._M_start = __tmp;
	  this->_M_impl._M_finish = __tmp + __old_size;
	  this->_M_impl._M_end_of_storage = this->_M_impl._M_start + __n;
	}
    }

  template<typename _Tp, typename _Alloc>
    typename vector<_Tp, _Alloc>::iterator
    vector<_Tp, _Alloc>::
    insert(iterator __position, const value_type& __x)
    {
      const size_type __n = __position - begin();
      if (this->_M_impl._M_finish != this->_M_impl._M_end_of_storage
	  && __position == end())
	{
	  this->_M_impl.construct(this->_M_impl._M_finish, __x);
	  ++this->_M_impl._M_finish;
	}
      else
        _M_insert_aux(__position, __x);
      return begin() + __n;
    }

  template<typename _Tp, typename _Alloc>
    typename vector<_Tp, _Alloc>::iterator
    vector<_Tp, _Alloc>::
    erase(iterator __position)
    {
      if (__position + 1 != end())
        std::copy(__position + 1, end(), __position);
      --this->_M_impl._M_finish;
      this->_M_impl.destroy(this->_M_impl._M_finish);
      return __position;
    }

  template<typename _Tp, typename _Alloc>
    typename vector<_Tp, _Alloc>::iterator
    vector<_Tp, _Alloc>::
    erase(iterator __first, iterator __last)
    {
      iterator __i(copy(__last, end(), __first));
      std::_Destroy(__i, end(), this->get_allocator());
      this->_M_impl._M_finish = this->_M_impl._M_finish - (__last - __first);
      return __first;
    }

  template<typename _Tp, typename _Alloc>
    vector<_Tp, _Alloc>&
    vector<_Tp, _Alloc>::
    operator=(const vector<_Tp, _Alloc>& __x)
    {
      if (&__x != this)
	{
	  const size_type __xlen = __x.size();
	  if (__xlen > capacity())
	    {
	      pointer __tmp = _M_allocate_and_copy(__xlen, __x.begin(),
						   __x.end());
	      std::_Destroy(this->_M_impl._M_start, this->_M_impl._M_finish,
			    this->get_allocator());
	      _M_deallocate(this->_M_impl._M_start,
			    this->_M_impl._M_end_of_storage
			    - this->_M_impl._M_start);
	      this->_M_impl._M_start = __tmp;
	      this->_M_impl._M_end_of_storage = this->_M_impl._M_start + __xlen;
	    }
	  else if (size() >= __xlen)
	    {
	      iterator __i(copy(__x.begin(), __x.end(), begin()));
	      std::_Destroy(__i, end(), this->get_allocator());
	    }
	  else
	    {
	      std::copy(__x.begin(), __x.begin() + size(),
			this->_M_impl._M_start);
	      std::__uninitialized_copy_a(__x.begin() + size(),
					  __x.end(), this->_M_impl._M_finish,
					  this->get_allocator());
	    }
	  this->_M_impl._M_finish = this->_M_impl._M_start + __xlen;
	}
      return *this;
    }

  template<typename _Tp, typename _Alloc>
    void
    vector<_Tp, _Alloc>::
    _M_fill_assign(size_t __n, const value_type& __val)
    {
      if (__n > capacity())
	{
	  vector __tmp(__n, __val, get_allocator());
	  __tmp.swap(*this);
	}
      else if (__n > size())
	{
	  std::fill(begin(), end(), __val);
	  std::__uninitialized_fill_n_a(this->_M_impl._M_finish,
					__n - size(), __val,
					this->get_allocator());
	  this->_M_impl._M_finish += __n - size();
	}
      else
        erase(fill_n(begin(), __n, __val), end());
    }

  template<typename _Tp, typename _Alloc>
    template<typename _InputIterator>
      void
      vector<_Tp, _Alloc>::
      _M_assign_aux(_InputIterator __first, _InputIterator __last,
		    input_iterator_tag)
      {
	iterator __cur(begin());
	for (; __first != __last && __cur != end(); ++__cur, ++__first)
	  *__cur = *__first;
	if (__first == __last)
	  erase(__cur, end());
	else
	  insert(end(), __first, __last);
      }

  template<typename _Tp, typename _Alloc>
    template<typename _ForwardIterator>
      void
      vector<_Tp, _Alloc>::
      _M_assign_aux(_ForwardIterator __first, _ForwardIterator __last,
		    forward_iterator_tag)
      {
	const size_type __len = std::distance(__first, __last);

	if (__len > capacity())
	  {
	    pointer __tmp(_M_allocate_and_copy(__len, __first, __last));
	    std::_Destroy(this->_M_impl._M_start, this->_M_impl._M_finish,
			  this->get_allocator());
	    _M_deallocate(this->_M_impl._M_start,
			  this->_M_impl._M_end_of_storage
			  - this->_M_impl._M_start);
	    this->_M_impl._M_start = __tmp;
	    this->_M_impl._M_finish = this->_M_impl._M_start + __len;
	    this->_M_impl._M_end_of_storage = this->_M_impl._M_finish;
	  }
	else if (size() >= __len)
	  {
	    iterator __new_finish(copy(__first, __last,
				       this->_M_impl._M_start));
	    std::_Destroy(__new_finish, end(), this->get_allocator());
	    this->_M_impl._M_finish = __new_finish.base();
	  }
	else
	  {
	    _ForwardIterator __mid = __first;
	    std::advance(__mid, size());
	    std::copy(__first, __mid, this->_M_impl._M_start);
	    this->_M_impl._M_finish =
	      std::__uninitialized_copy_a(__mid, __last,
					  this->_M_impl._M_finish,
					  this->get_allocator());
	  }
      }

  template<typename _Tp, typename _Alloc>
    void
    vector<_Tp, _Alloc>::
    _M_insert_aux(iterator __position, const _Tp& __x)
    {
      if (this->_M_impl._M_finish != this->_M_impl._M_end_of_storage)
	{
	  this->_M_impl.construct(this->_M_impl._M_finish,
				  *(this->_M_impl._M_finish - 1));
	  ++this->_M_impl._M_finish;
	  _Tp __x_copy = __x;
	  std::copy_backward(__position,
			     iterator(this->_M_impl._M_finish-2),
			     iterator(this->_M_impl._M_finish-1));
	  *__position = __x_copy;
	}
      else
	{
	  const size_type __old_size = size();
	  const size_type __len = __old_size != 0 ? 2 * __old_size : 1;
	  iterator __new_start(this->_M_allocate(__len));
	  iterator __new_finish(__new_start);
	  try
	    {
	      __new_finish =
		std::__uninitialized_copy_a(iterator(this->_M_impl._M_start),
					    __position,
					    __new_start,
					    this->get_allocator());
	      this->_M_impl.construct(__new_finish.base(), __x);
	      ++__new_finish;
	      __new_finish =
		std::__uninitialized_copy_a(__position,
					    iterator(this->_M_impl._M_finish),
					    __new_finish,
					    this->get_allocator());
          }
	  catch(...)
	    {
	      std::_Destroy(__new_start, __new_finish, this->get_allocator());
	      _M_deallocate(__new_start.base(),__len);
	      __throw_exception_again;
	    }
	  std::_Destroy(begin(), end(), this->get_allocator());
	  _M_deallocate(this->_M_impl._M_start,
			this->_M_impl._M_end_of_storage
			- this->_M_impl._M_start);
	  this->_M_impl._M_start = __new_start.base();
	  this->_M_impl._M_finish = __new_finish.base();
	  this->_M_impl._M_end_of_storage = __new_start.base() + __len;
	}
    }

  template<typename _Tp, typename _Alloc>
    void
    vector<_Tp, _Alloc>::
    _M_fill_insert(iterator __position, size_type __n, const value_type& __x)
    {
      if (__n != 0)
	{
	  if (size_type(this->_M_impl._M_end_of_storage
			- this->_M_impl._M_finish) >= __n)
	    {
	      value_type __x_copy = __x;
	      const size_type __elems_after = end() - __position;
	      iterator __old_finish(this->_M_impl._M_finish);
	      if (__elems_after > __n)
		{
		  std::__uninitialized_copy_a(this->_M_impl._M_finish - __n,
					      this->_M_impl._M_finish,
					      this->_M_impl._M_finish,
					      this->get_allocator());
		  this->_M_impl._M_finish += __n;
		  std::copy_backward(__position, __old_finish - __n,
				     __old_finish);
		  std::fill(__position, __position + __n, __x_copy);
		}
	      else
		{
		  std::__uninitialized_fill_n_a(this->_M_impl._M_finish,
						__n - __elems_after,
						__x_copy,
						this->get_allocator());
		  this->_M_impl._M_finish += __n - __elems_after;
		  std::__uninitialized_copy_a(__position, __old_finish,
					      this->_M_impl._M_finish,
					      this->get_allocator());
		  this->_M_impl._M_finish += __elems_after;
		  std::fill(__position, __old_finish, __x_copy);
		}
	    }
	  else
	    {
	      const size_type __old_size = size();
	      const size_type __len = __old_size + std::max(__old_size, __n);
	      iterator __new_start(this->_M_allocate(__len));
	      iterator __new_finish(__new_start);
	      try
		{
		  __new_finish =
		    std::__uninitialized_copy_a(begin(), __position,
						__new_start,
						this->get_allocator());
		  std::__uninitialized_fill_n_a(__new_finish, __n, __x,
						this->get_allocator());
		  __new_finish += __n;
		  __new_finish =
		    std::__uninitialized_copy_a(__position, end(), __new_finish,
						this->get_allocator());
		}
	      catch(...)
		{
		  std::_Destroy(__new_start, __new_finish,
				this->get_allocator());
		  _M_deallocate(__new_start.base(), __len);
		  __throw_exception_again;
		}
	      std::_Destroy(this->_M_impl._M_start, this->_M_impl._M_finish,
			    this->get_allocator());
	      _M_deallocate(this->_M_impl._M_start,
			    this->_M_impl._M_end_of_storage
			    - this->_M_impl._M_start);
	      this->_M_impl._M_start = __new_start.base();
	      this->_M_impl._M_finish = __new_finish.base();
	      this->_M_impl._M_end_of_storage = __new_start.base() + __len;
	    }
	}
    }

  template<typename _Tp, typename _Alloc> template<typename _InputIterator>
    void
    vector<_Tp, _Alloc>::
    _M_range_insert(iterator __pos, _InputIterator __first,
		    _InputIterator __last, input_iterator_tag)
    {
      for (; __first != __last; ++__first)
	{
	  __pos = insert(__pos, *__first);
	  ++__pos;
	}
    }

  template<typename _Tp, typename _Alloc>
    template<typename _ForwardIterator>
      void
      vector<_Tp, _Alloc>::
      _M_range_insert(iterator __position,_ForwardIterator __first,
		      _ForwardIterator __last, forward_iterator_tag)
      {
	if (__first != __last)
	  {
	    const size_type __n = std::distance(__first, __last);
	    if (size_type(this->_M_impl._M_end_of_storage
			  - this->_M_impl._M_finish) >= __n)
	      {
		const size_type __elems_after = end() - __position;
		iterator __old_finish(this->_M_impl._M_finish);
		if (__elems_after > __n)
		  {
		    std::__uninitialized_copy_a(this->_M_impl._M_finish - __n,
						this->_M_impl._M_finish,
						this->_M_impl._M_finish,
						this->get_allocator());
		    this->_M_impl._M_finish += __n;
		    std::copy_backward(__position, __old_finish - __n,
				       __old_finish);
		    std::copy(__first, __last, __position);
		  }
		else
		  {
		    _ForwardIterator __mid = __first;
		    std::advance(__mid, __elems_after);
		    std::__uninitialized_copy_a(__mid, __last,
						this->_M_impl._M_finish,
						this->get_allocator());
		    this->_M_impl._M_finish += __n - __elems_after;
		    std::__uninitialized_copy_a(__position, __old_finish,
						this->_M_impl._M_finish,
						this->get_allocator());
		    this->_M_impl._M_finish += __elems_after;
		    std::copy(__first, __mid, __position);
		  }
	      }
	    else
	      {
		const size_type __old_size = size();
		const size_type __len = __old_size + std::max(__old_size, __n);
		iterator __new_start(this->_M_allocate(__len));
		iterator __new_finish(__new_start);
		try
		  {
		    __new_finish =
		      std::__uninitialized_copy_a(iterator(this->_M_impl._M_start),
						  __position,
						  __new_start,
						  this->get_allocator());
		    __new_finish =
		      std::__uninitialized_copy_a(__first, __last, __new_finish,
						  this->get_allocator());
		    __new_finish =
		      std::__uninitialized_copy_a(__position,
						  iterator(this->_M_impl._M_finish),
						  __new_finish,
						  this->get_allocator());
		  }
		catch(...)
		  {
		    std::_Destroy(__new_start,__new_finish,
				  this->get_allocator());
		    _M_deallocate(__new_start.base(), __len);
		    __throw_exception_again;
		  }
		std::_Destroy(this->_M_impl._M_start, this->_M_impl._M_finish,
			      this->get_allocator());
		_M_deallocate(this->_M_impl._M_start,
			      this->_M_impl._M_end_of_storage
			      - this->_M_impl._M_start);
		this->_M_impl._M_start = __new_start.base();
		this->_M_impl._M_finish = __new_finish.base();
		this->_M_impl._M_end_of_storage = __new_start.base() + __len;
	      }
	  }
      }
} // namespace std

#endif /* _VECTOR_TCC */
