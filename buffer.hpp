
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace jup {

// Forward declaration of Buffer, for the constructor of Buffer_view.
struct Buffer;

/**
 * A read only objects referencing a continuous memory region of a certain size
 * with arbitrary contents. Supports iteration over the bytes. Has no ownership
 * of any kind. This is a simple pointer + size combination.
 *
 * If you want to story a c-style string in here, use the size of the string
 * without the terminating zero and call c_str() to extract the string, instead
 * of data().
 */
struct Buffer_view {
	Buffer_view(void const* data = nullptr, int size = 0):
		m_data{data}, m_size{size} {assert(size >= 0);}
	
	Buffer_view(Buffer const& buf);
	
	template<typename T>
	Buffer_view(std::vector<T> const& vec):
		Buffer_view{vec.data(), (int)(vec.size() * sizeof(T))} {}
	
	template<typename T>
	Buffer_view(std::basic_string<T> const& str):
		Buffer_view{str.data(), (int)(str.size() * sizeof(T))} {}
	
	Buffer_view(char const* str):
		Buffer_view{str, (int)std::strlen(str)} {}

	/**
	 * Construct from arbitrary object. This is not a constructor due to the
	 * obvious overloading problems.
	 */
	template<typename T>
	static Buffer_view from_obj(T const& obj) {
		return Buffer_view {&obj, sizeof(obj)};
	}

	int size() const { return m_size; }
	
	char const* begin() const { return (char const*)m_data; }
	char const* end()   const { return (char const*)m_data + m_size; }
	char const* data()  const { return begin(); }

	/**
	 * Provide access to the bytes, with bounds checking.
	 */
	char operator[] (int pos) const {
		assert(0 <= pos and pos < size());
		return data()[pos];
	}

	/**
	 * Return a c-style string. Same as data, but asserts that the character
	 * just behind the last one is zero.
	 */
	char const* c_str() const {
		assert(*(data() + size()) == 0);
		return data();
	}

	/**
	 * Generate a simple hash of the contents of this Buffer_view. An empty
	 * buffer must have a hash of 0.
	 */
	u32 get_hash() const {
		u32 result = 0;
		for (char c: *this) {
			result = (result * 33) ^ c;
		}
		return result;
	}

	/**
	 * Compare for byte-wise equality. 
	 */
	bool operator== (Buffer_view const& buf) const {
		if (size() != buf.size()) return false;
		for (int i = 0; i < size(); ++i) {
			if ((*this)[i] != buf[i]) return false;
		}
		return true;
	}
	bool operator!= (Buffer_view const& buf) const { return !(*this == buf); }
	
	void const* const m_data;
	int const m_size;
};

/**
 * A handle for a continuous region of memory that can dynamically expand, if
 * necessary. This is like std::vector<char> in many regards. It supports both
 * move and copy semantics and has ownership of the managed memory (meaning that
 * the memory is free'd an destruction). There are no guarantees made for the
 * contents of uninitialized memory.
 *
 * There are three member variables:
 *  - the pointer to the memory, data()
 *  - the size of the allocated memory, capacity()
 *  - the amount of the memory that is used, size()
 * These are used together in the various methods. Of course, you may decide to
 * disregard size() completely and just use the block of memory.
 *
 * In debug mode (NDEBUG not defined) you can trap pointer invalidation due to
 * resizing.
 */
class Buffer {
#ifndef NDEBUG
	static_assert(sizeof(int) == 4, "Assuming 32bit ints for the bitmasks.");
#endif
public:
	/**
	 * These do what you would expect them to.
	 */
	Buffer() {}
	Buffer(Buffer const& buf) { append(buf); }
	Buffer(Buffer&& buf) {
		m_data = buf.m_data;
		m_size = buf.m_size;
		m_capacity = buf.m_capacity;
		buf.m_data = nullptr;
		buf.m_size = 0;
		buf.m_capacity = 0;
	}
	~Buffer() { free(); }
	Buffer& operator= (Buffer const& buf) {
		reset();
		append(buf);
		return *this;
	}
	Buffer& operator= (Buffer&& buf) {
		std::swap(m_data, buf.m_data);
		std::swap(m_size, buf.m_size);
		std::swap(m_capacity, buf.m_capacity);
		return *this;
	}

	/**
	 * Ensure that the Buffer has a capacity of at least newcap. If the current
	 * capacity is bigger, this does nothing. Else, new memory is allocated and
	 * the contents of the current block are moved. The new capacity is at least
	 * twice the old one.
	 */
	void reserve(int newcap) {
		if (capacity() < newcap) {
			assert(!trap_alloc());
			newcap = std::max(newcap, capacity() * 2);
			if (m_data) {
				m_data = (char*)std::realloc(m_data, newcap);
			} else {
				m_data = (char*)std::malloc(newcap);
			}
			// the trap_alloc flag is stored in m_capacity, don't disturb it
			m_capacity += newcap - capacity();
			assert(m_data);
		}
	}

	/**
	 * Append the contents of the memory to this buffer.
	 */
	void append(void const* buf, int buf_size) {
		if (!buf_size) return;
		assert(buf_size > 0 and buf);
		if (capacity() < m_size + buf_size)
			reserve(m_size + buf_size);
		
		assert(capacity() >= m_size + buf_size);
		std::memcpy(m_data + m_size, buf, buf_size);
		m_size += buf_size;
	}
	void append(Buffer_view buffer) {
		append(buffer.data(), buffer.size());
	}

	/**
	 * Change the size of the Buffer. Useful if you write to the memory
	 * manually.
	 */
	void resize(int nsize) {
		m_size = nsize;
		assert(m_size >= 0);
		reserve(m_size);
	}
	void addsize(int incr) {
		resize(m_size + incr);
	}

	/**
	 * Set the size to 0. Do not confuse this with free(), this does not
	 * actually release the memory.
	 */
	void reset() {
		m_size = 0;
	}

	/**
	 * Free the memory. Leaves the buffer in a valid state.
	 */
	void free() {
		assert(!trap_alloc());
		std::free(m_data);
		m_data = nullptr;
		m_size = 0;
		m_capacity = 0;
	}

	int size() const { return m_size; }
	int capacity() const {
		// If in debug mode, the most-significant bit of m_capacity is serving
		// as the trap_alloc flag.
#ifndef NDEBUG
		return m_capacity & 0x7fffffff;
#else
		return m_capacity;
#endif
	}

	/**
	 * Returns whether any pointer invalidation for pointers into the buffer may
	 * occur. If this is set, the program will abort on reallocation, which is
	 * useful for debugging.
	 */
	bool trap_alloc() const {
#ifndef NDEBUG
		return (u32)m_capacity >> 31;
#else
		return false;
#endif
	}

	/**
	 * Set the trap_alloc() flag.
	 */
	bool trap_alloc(bool value) {
#ifndef NDEBUG
		m_capacity ^= (u32)(trap_alloc() ^ value) << 31;
#endif					   
		return trap_alloc();
	}

	/**
	 * space() is the amount of space left in the Buffer (the capacity minus the
	 * size).
	 */
	int space() const {return capacity() - size();}
	/**
	 * Ensure, that atleast space is available.
	 */
	void reserve_space(int atleast) {
		reserve(size() + atleast);
	}

	/**
	 * A helper to save you some casting. Returns the memory offset by offset
	 * bytes interpreted as a T. Ensures that that much memory is
	 * available. This is of course not type-safe in any way, shape, or form.
	 */
	template <typename T>
	T& get(int offset = 0) {
		reserve(offset + sizeof(T));
		return *(T*)(m_data + offset);
	}
	/**
	 * Like get, but constructs the object in-place.
	 */
	template <typename T, typename... Args>
	T& emplace(int offset = 0, Args&&... args) {
		int end = offset + sizeof(T);
		reserve(end);
		if (m_size < end) resize(end);
		return *(new(m_data + offset) T {std::forward<Args>(args)...});
	}
	/**
	 * Like emplace, but contructs the object at the end.
	 */
	template <typename T, typename... Args>
	T& emplace_back(Args&&... args) {
		return emplace<T>(size(), std::forward<Args>(args)...);
	}

	char* begin() {return m_data;}
	char* end()   {return m_data + m_size;}
	char* data()  {return begin();}
	char const* begin() const {return m_data;}
	char const* end()   const {return m_data + m_size;}
	char const* data()  const {return begin();}

private:
	char* m_data = nullptr;
	int m_size = 0, m_capacity = 0;
};


inline Buffer_view::Buffer_view(Buffer const& buf):
	Buffer_view{buf.data(), buf.size()} {}

} /* end of namespace jup */
