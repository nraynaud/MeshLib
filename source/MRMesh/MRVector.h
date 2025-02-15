#pragma once

#include "MRMeshFwd.h"
#include <cassert>
#include <vector>

namespace MR
{

/**
 * \brief std::vector<T>-like container that requires specific indexing type,
 * \tparam T type of stored elements
 * \tparam I type of index (shall be convertible to size_t)
 * \ingroup BasicGroup
 */
template <typename T, typename I>
class Vector
{
public:
    using reference = typename std::vector<T>::reference;
    using const_reference = typename std::vector<T>::const_reference;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    Vector() = default;
    explicit Vector( size_t size ) : vec_( size ) { }
    explicit Vector( size_t size, const T & val ) : vec_( size, val ) { }
    Vector( std::vector<T> && vec ) : vec_( std::move( vec ) ) { }
    template< class InputIt >
    Vector( InputIt first, InputIt last ) : vec_( first, last ) { }
    Vector( std::initializer_list<T> init ) : vec_( init ) { }

    [[nodiscard]] bool operator == ( const Vector & b ) const { return vec_ == b.vec_; }
    [[nodiscard]] bool operator != ( const Vector & b ) const { return vec_ != b.vec_; }

    void clear() { vec_.clear(); }
    [[nodiscard]] bool empty() const { return vec_.empty(); }

    [[nodiscard]] auto size() const { return vec_.size(); }

    void resize( size_t newSize ) { vec_.resize( newSize ); }
    void resize( size_t newSize, const T & t ) { vec_.resize( newSize, t ); }

    [[nodiscard]] auto capacity() const { return vec_.capacity(); }
    void reserve( size_t capacity ) { vec_.reserve( capacity ); }

    [[nodiscard]] const_reference operator[]( I i ) const
    {
        assert( i < vec_.size() );
        return vec_[i];
    }
    [[nodiscard]] reference operator[]( I i )
    {
        assert( i < vec_.size() );
        return vec_[i];
    }

    /// doubles reserved memory until resize(newSize) can be done without reallocation
    void resizeWithReserve( size_t newSize )
    {
        auto reserved = vec_.capacity();
        if ( reserved > 0 && newSize > reserved )
        {
            while ( newSize > reserved )
                reserved <<= 1;
            vec_.reserve( reserved );
        }
        vec_.resize( newSize );
    }

    /// this accessor automatically adjusts the size of the vector
    [[nodiscard]] reference autoResizeAt( I i )
    {
        if ( i + 1 > size() )
            resizeWithReserve( i + 1 );
        return vec_[i];
    }

    void push_back( const T & t ) { vec_.push_back( t ); }
    void push_back( T && t ) { vec_.push_back( std::move( t ) ); }
    void pop_back() { vec_.pop_back(); }

    template<typename... Args>
    auto emplace_back( Args&&... args ) { return vec_.emplace_back( std::forward<Args>(args)... ); }

    [[nodiscard]] const_reference front() const { return vec_.front(); }
    [[nodiscard]]       reference front()       { return vec_.front(); }
    [[nodiscard]] const_reference  back() const { return vec_.back(); }
    [[nodiscard]]       reference  back()       { return vec_.back(); }
    /// returns the identifier of the back() element
    [[nodiscard]] I backId() const { assert( !vec_.empty() ); return I{ vec_.size() - 1 }; }
    /// returns backId() + 1
    [[nodiscard]] I endId() const { return I{ vec_.size() }; }

    [[nodiscard]] auto data() { return vec_.data(); }
    [[nodiscard]] auto data() const { return vec_.data(); }

    void swap( Vector & b ) { vec_.swap( b.vec_ ); }

    /// returns the amount of memory this object occupies on heap
    [[nodiscard]] size_t heapBytes() const { return capacity() * sizeof(T); }

    /// the user can directly manipulate the vector, anyway she cannot break anything
    std::vector<T> vec_;
};

template <typename T, typename I>
[[nodiscard]] inline auto begin( const Vector<T, I> & a )
    { return a.vec_.begin(); }

template <typename T, typename I>
[[nodiscard]] inline auto begin( Vector<T, I> & a )
    { return a.vec_.begin(); }

template <typename T, typename I>
[[nodiscard]] inline auto end( const Vector<T, I> & a )
    { return a.vec_.end(); }

template <typename T, typename I>
[[nodiscard]] inline auto end( Vector<T, I> & a )
    { return a.vec_.end(); }

} // namespace MR
