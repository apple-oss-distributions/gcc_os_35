/* BufferedInputStream.java -- An input stream that implements buffering
   Copyright (C) 1998, 1999, 2001 Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */

 
package java.io;

/* Written using "Java Class Libraries", 2nd edition, ISBN 0-201-31002-3
 * "The Java Language Specification", ISBN 0-201-63451-1
 * plus online API docs for JDK 1.2 beta from http://www.javasoft.com.
 * Status:  Believed complete and correct.
 */
 
/**
 * This subclass of <code>FilterInputStream</code> buffers input from an 
 * underlying implementation to provide a possibly more efficient read
 * mechanism.  It maintains the buffer and buffer state in instance 
 * variables that are available to subclasses.  The default buffer size
 * of 2048 bytes can be overridden by the creator of the stream.
 * <p>
 * This class also implements mark/reset functionality.  It is capable
 * of remembering any number of input bytes, to the limits of
 * system memory or the size of <code>Integer.MAX_VALUE</code>
 * <p>
 * Please note that this class does not properly handle character
 * encodings.  Consider using the <code>BufferedReader</code> class which
 * does.
 *
 * @author Aaron M. Renn (arenn@urbanophile.com)
 * @author Warren Levy <warrenl@cygnus.com>
 * @author Jeroen Frijters <jeroen@frijters.net>
 */
public class BufferedInputStream extends FilterInputStream
{

  /**
   * This is the default buffer size
   */
  private static final int DEFAULT_BUFFER_SIZE = 2048;

  /**
   * The buffer used for storing data from the underlying stream.
   */
  protected byte[] buf;

  /**
   * The number of valid bytes currently in the buffer.  It is also the index
   * of the buffer position one byte past the end of the valid data.
   */
  protected int count;

  /**
   * The index of the next character that will by read from the buffer.
   * When <code>pos == count</code>, the buffer is empty.
   */
  protected int pos;

  /**
   * The value of <code>pos</code> when the <code>mark()</code> method was
   * called.  
   * This is set to -1 if there is no mark set.
   */
  protected int markpos = -1;

  /**
   * This is the maximum number of bytes than can be read after a 
   * call to <code>mark()</code> before the mark can be discarded.
   * After this may bytes are read, the <code>reset()</code> method
   * may not be called successfully.
   */
  protected int marklimit;

  /**
   * This is the maximum size we have to allocate for the mark buffer.
   * This number may be huge (Integer.MAX_VALUE). The class will continue
   * to allocate new chunks (specified by <code>CHUNKSIZE</code>) until the
   * the size specified by this field is achieved.
   */
  private int marktarget = 0;

  /**
   * This is the number of bytes to allocate to reach marktarget.
   */
  static final private int CHUNKSIZE = 1024;

  /**
   * This method initializes a new <code>BufferedInputStream</code> that will
   * read from the specified subordinate stream with a default buffer size
   * of 2048 bytes
   *
   * @param in The subordinate stream to read from
   */
  public BufferedInputStream(InputStream in)
  {
    this(in, DEFAULT_BUFFER_SIZE);
  }

  /**
   * This method initializes a new <code>BufferedInputStream</code> that will
   * read from the specified subordinate stream with a buffer size that
   * is specified by the caller.
   *
   * @param in The subordinate stream to read from
   * @param size The buffer size to use
   *
   * @exception IllegalArgumentException when size is smaller then 1
   */
  public BufferedInputStream(InputStream in, int size)
  {
    super(in);
    if (size <= 0)
      throw new IllegalArgumentException();
    buf = new byte[size];
  }

  /**
   * This method returns the number of bytes that can be read from this
   * stream before a read can block.  A return of 0 indicates that blocking
   * might (or might not) occur on the very next read attempt.
   * <p>
   * The number of available bytes will be the number of read ahead bytes 
   * stored in the internal buffer plus the number of available bytes in
   * the underlying stream.
   *
   * @return The number of bytes that can be read before blocking could occur
   *
   * @exception IOException If an error occurs
   */
  public synchronized int available() throws IOException
  {
    return count - pos + super.available();
  }

  /**
   * This method closes the underlying input stream and frees any
   * resources associated with it. Sets <code>buf</code> to <code>null</code>.
   *
   * @exception IOException If an error occurs.
   */
  public void close() throws IOException
  {
    // Free up the array memory.
    buf = null;
    super.close();
  }

  /**
   * This method marks a position in the input to which the stream can be
   * "reset" by calling the <code>reset()</code> method.  The parameter
   * <code>readlimit</code> is the number of bytes that can be read from the 
   * stream after setting the mark before the mark becomes invalid.  For
   * example, if <code>mark()</code> is called with a read limit of 10, then
   * when 11 bytes of data are read from the stream before the
   * <code>reset()</code> method is called, then the mark is invalid and the
   * stream object instance is not required to remember the mark.
   * <p>
   * Note that the number of bytes that can be remembered by this method
   * can be greater than the size of the internal read buffer.  It is also
   * not dependent on the subordinate stream supporting mark/reset
   * functionality.
   *
   * @param readlimit The number of bytes that can be read before the mark
   * becomes invalid
   */
  public synchronized void mark(int readlimit)
  {
    marktarget = marklimit = readlimit;
    if (marklimit > CHUNKSIZE)
	marklimit = CHUNKSIZE;
    markpos = pos;
  }

  /**
   * This method returns <code>true</code> to indicate that this class
   * supports mark/reset functionality.
   *
   * @return <code>true</code> to indicate that mark/reset functionality is
   * supported
   *
   */
  public boolean markSupported()
  {
    return true;
  }

  /**
   * This method reads an unsigned byte from the input stream and returns it
   * as an int in the range of 0-255.  This method also will return -1 if
   * the end of the stream has been reached.
   * <p>
   * This method will block until the byte can be read.
   *
   * @return The byte read or -1 if end of stream
   *
   * @exception IOException If an error occurs
   */
  public synchronized int read() throws IOException
  {
    if (pos >= count && !refill())
      return -1;	// EOF

    if (markpos >= 0 && pos - markpos > marktarget)
      markpos = -1;

    return ((int) buf[pos++]) & 0xFF;
  }

  /**
   * This method reads bytes from a stream and stores them into a caller
   * supplied buffer.  It starts storing the data at index <code>off</code>
   * into the buffer and attempts to read <code>len</code> bytes.  This method
   * can return before reading the number of bytes requested.  The actual
   * number of bytes read is returned as an int.  A -1 is returned to indicate
   * the end of the stream.
   * <p>
   * This method will block until some data can be read.
   *
   * @param b The array into which the bytes read should be stored
   * @param off The offset into the array to start storing bytes
   * @param len The requested number of bytes to read
   *
   * @return The actual number of bytes read, or -1 if end of stream.
   *
   * @exception IOException If an error occurs.
   * @exception IndexOutOfBoundsException when <code>off</code> or
   *            <code>len</code> are negative, or when <code>off + len</code>
   *            is larger then the size of <code>b</code>,
   */
  public synchronized int read(byte[] b, int off, int len) throws IOException
  {
    if (off < 0 || len < 0 || b.length - off < len)
      throw new IndexOutOfBoundsException();

    if (pos >= count && !refill())
      return -1;		// No bytes were read before EOF.

    int remain = Math.min(count - pos, len);
    System.arraycopy(buf, pos, b, off, remain);
    pos += remain;

    if (markpos >= 0 && pos - markpos > marktarget)
      markpos = -1;

    return remain;
  }

  /**
   * This method resets a stream to the point where the <code>mark()</code>
   * method was called.  Any bytes that were read after the mark point was
   * set will be re-read during subsequent reads.
   * <p>
   * This method will throw an IOException if the number of bytes read from
   * the stream since the call to <code>mark()</code> exceeds the mark limit
   * passed when establishing the mark.
   *
   * @exception IOException If <code>mark()</code> was never called or more
   *            then <code>marklimit</code> bytes were read since the last
   *            call to <code>mark()</code>
   */
  public synchronized void reset() throws IOException
  {
    if (markpos == -1)
      throw new IOException(buf == null ? "Stream closed." : "Invalid mark.");

    pos = markpos;
  }

  /**
   * This method skips the specified number of bytes in the stream.  It
   * returns the actual number of bytes skipped, which may be less than the
   * requested amount.
   *
   * @param n The requested number of bytes to skip
   *
   * @return The actual number of bytes skipped.
   *
   * @exception IOException If an error occurs
   */
  public synchronized long skip(long n) throws IOException
  {
    if (buf == null)
	throw new IOException("Stream closed.");

    final long origN = n;

    while (n > 0L)
      {
	if (pos >= count && !refill())
	  if (n < origN)
	    break;
	  else
	    return -1;	// No bytes were read before EOF.

	int numread = (int) Math.min((long) (count - pos), n);
	pos += numread;
	n -= numread;

        if (markpos >= 0 && pos - markpos > marktarget)
          markpos = -1;
      }

    return origN - n;
  }

  /**
   * Called to refill the buffer (when count is equal to pos).
   *
   * @return <code>true</code> when at least one additional byte was read
   *         into <code>buf</code>, <code>false</code> otherwise (at EOF).
   */
  boolean refill() throws IOException
  {
    if (buf == null)
	throw new IOException("Stream closed.");

    if (markpos < 0)
      count = pos = 0;
    else if (markpos > 0)
      {
        // Shift the marked bytes (if any) to the beginning of the array
	// but don't grow it.  This saves space in case a reset is done
	// before we reach the max capacity of this array.
        System.arraycopy(buf, markpos, buf, 0, count - markpos);
	count -= markpos;
	pos -= markpos;
	markpos = 0;
      }
    else if (marktarget >= buf.length && marklimit < marktarget)	// BTW, markpos == 0
      {
	// Need to grow the buffer now to have room for marklimit bytes.
	// Note that the new buffer is one greater than marklimit.
	// This is so that there will be one byte past marklimit to be read
	// before having to call refill again, thus allowing marklimit to be
	// invalidated.  That way refill doesn't have to check marklimit.
	marklimit += CHUNKSIZE;
	if (marklimit >= marktarget)
	  marklimit = marktarget;
	byte[] newbuf = new byte[marklimit + 1];
	System.arraycopy(buf, 0, newbuf, 0, count);
	buf = newbuf;
      }

    int numread = super.read(buf, count, buf.length - count);

    if (numread < 0)	// EOF
      return false;

    count += numread;
    return true;
  }
}
