#pragma once

#include <DB/Common/HashTable/HashTable.h>


/** Двухуровневая хэш-таблица.
  * Представляет собой 256 маленьких хэш-таблиц (bucket-ов первого уровня).
  * Для определения, какую из них использовать, берётся один из байтов хэш-функции.
  *
  * Обычно работает чуть-чуть медленнее простой хэш-таблицы.
  * Тем не менее, обладает преимуществами в некоторых случаях:
  * - если надо мерджить две хэш-таблицы вместе, то это можно легко распараллелить по bucket-ам;
  * - лаг при ресайзах размазан, так как маленькие хэш-таблицы ресайзятся по-отдельности;
  * - по идее, ресайзы кэш-локальны в большем диапазоне размеров.
  */

template
<
	typename Key,
	typename Cell,
	typename Hash,
	typename Grower,
	typename Allocator,
	typename ImplTable = HashTable<Key, Cell, Hash, Grower, Allocator>
>
class TwoLevelHashTable :
	private boost::noncopyable,
	protected Hash			/// empty base optimization
{
protected:
	friend class const_iterator;
	friend class iterator;

	typedef size_t HashValue;
	typedef TwoLevelHashTable<Key, Cell, Hash, Grower, Allocator> Self;
	typedef ImplTable Impl;

	size_t m_size = 0;		/// Количество элементов

	size_t hash(const Key & x) const { return Hash::operator()(x); }
	size_t bucket(size_t hash_value) const { return hash_value >> 24; }	/// TODO: брать не настолько младший байт.

	typename Impl::iterator beginOfNextNonEmptyBucket(size_t & bucket)
	{
		do
		{
			++bucket;
		}
		while (bucket != NUM_BUCKETS && !impls[bucket].empty());

		if (bucket != NUM_BUCKETS)
			return impls[bucket].begin();

		return impls[NUM_BUCKETS - 1].end();
	}

	typename Impl::const_iterator beginOfNextNonEmptyBucket(size_t & bucket) const
	{
		do
		{
			++bucket;
		}
		while (bucket != NUM_BUCKETS && !impls[bucket].empty());

		if (bucket != NUM_BUCKETS)
			return impls[bucket].begin();

		return impls[NUM_BUCKETS - 1].end();
	}

public:
	typedef typename Impl::key_type key_type;
	typedef typename Impl::value_type value_type;

	static constexpr size_t NUM_BUCKETS = 256;
	Impl impls[NUM_BUCKETS];


	class iterator
	{
		Impl * impls;
		size_t bucket;
		typename Impl::iterator current_it;

		friend class TwoLevelHashTable;

		iterator(Impl * impls_, size_t bucket_, typename Impl::iterator & current_it_)
			: impls(impls_), bucket(bucket_), current_it(current_it_) {}

	public:
		iterator() {}

		bool operator== (const iterator & rhs) const { return current_it == rhs.current_it; }
		bool operator!= (const iterator & rhs) const { return current_it != rhs.current_it; }

		iterator & operator++()
		{
			++current_it;
			if (current_it == impls[bucket].end())
				current_it = beginOfNextNonEmptyBucket(bucket);

			return *this;
		}

		value_type & operator* () const { return *current_it; }
		value_type * operator->() const { return &*current_it; }
	};


	class const_iterator
	{
		Impl * impls;
		size_t bucket;
		typename Impl::const_iterator current_it;

		friend class TwoLevelHashTable;

		const_iterator(Impl * impls_, size_t bucket_, typename Impl::const_iterator & current_it_)
			: impls(impls_), bucket(bucket_), current_it(current_it_) {}

	public:
		const_iterator() {}
		const_iterator(const iterator & rhs) : impls(rhs.impls), current_it(rhs.current_it), bucket(rhs.bucket) {}

		bool operator== (const const_iterator & rhs) const { return current_it == rhs.current_it; }
		bool operator!= (const const_iterator & rhs) const { return current_it != rhs.current_it; }

		const_iterator & operator++()
		{
			++current_it;
			if (current_it == impls[bucket].end())
				current_it = beginOfNextNonEmptyBucket(bucket);

			return *this;
		}

		const value_type & operator* () const { return *current_it; }
		const value_type * operator->() const { return &*current_it; }
	};


	const_iterator begin() const
	{
		size_t buck = 0;
		return beginOfNextNonEmptyBucket(buck);
	}

	iterator begin()
	{
		size_t buck = 0;
		return beginOfNextNonEmptyBucket(buck);
	}

	const_iterator end() const 		{ return impls[NUM_BUCKETS - 1].end(); }
	iterator end() 					{ return impls[NUM_BUCKETS - 1].end(); }


	/// Вставить значение. В случае хоть сколько-нибудь сложных значений, лучше используйте функцию emplace.
	std::pair<iterator, bool> insert(const value_type & x)
	{
		size_t hash_value = hash(Cell::getKey(x));

		std::pair<iterator, bool> res;
		emplace(Cell::getKey(x), res.first, res.second, hash_value);
		return res;
	}


	/** Вставить ключ,
	  * вернуть итератор на позицию, которую можно использовать для placement new значения,
	  * а также флаг - был ли вставлен новый ключ.
	  *
	  * Вы обязаны сделать placement new значения, если был вставлен новый ключ,
	  * так как при уничтожении хэш-таблицы для него будет вызываться деструктор!
	  *
	  * Пример использования:
	  *
	  * Map::iterator it;
	  * bool inserted;
	  * map.emplace(key, it, inserted);
	  * if (inserted)
	  * 	new(&it->second) Mapped(value);
	  */
	void emplace(Key x, iterator & it, bool & inserted)
	{
		size_t hash_value = hash(x);
		emplace(x, it, inserted, hash_value);
	}


	/// То же самое, но с заранее вычисленным значением хэш-функции.
	void emplace(Key x, iterator & it, bool & inserted, size_t hash_value)
	{
		size_t buck = bucket(hash_value);
		typename Impl::iterator impl_it;
		impls[buck].emplace(x, impl_it, inserted);
		it = iterator(impls, buck, impl_it);

		if (inserted)
			++m_size;
	}


	iterator find(Key x)
	{
		size_t hash_value = hash(x);
		size_t buck = bucket(hash_value);

		typename Impl::iterator found = impls[buck].find(x);
		return found != impls[buck].end()
			? iterator(impls, buck, found)
			: end();
	}


	const_iterator find(Key x) const
	{
		size_t hash_value = hash(x);
		size_t buck = bucket(hash_value);

		typename Impl::const_iterator found = impls[buck].find(x);
		return found != impls[buck].end()
			? const_iterator(impls, buck, found)
			: end();
	}


	void write(DB::WriteBuffer & wb) const
	{
		for (size_t i = 0; i < NUM_BUCKETS; ++i)
			impls[i].write(wb);
	}

	void writeText(DB::WriteBuffer & wb) const
	{
		for (size_t i = 0; i < NUM_BUCKETS; ++i)
		{
			if (i != 0)
				DB::writeChar(',', wb);
			impls[i].writeText(wb);
		}
	}

	void read(DB::ReadBuffer & rb)
	{
		for (size_t i = 0; i < NUM_BUCKETS; ++i)
			impls[i].read(rb);
	}

	void readText(DB::ReadBuffer & rb)
	{
		for (size_t i = 0; i < NUM_BUCKETS; ++i)
		{
			if (i != 0)
				DB::assertString(",", rb);
			impls[i].readText(rb);
		}
	}


	size_t size() const
	{
	    return m_size;
	}

	bool empty() const
	{
	    return 0 == m_size;
	}

	size_t getBufferSizeInBytes() const
	{
		size_t res;
		for (size_t i = 0; i < NUM_BUCKETS; ++i)
			res += impls[i].getBufferSizeInBytes();

		return res;
	}
};