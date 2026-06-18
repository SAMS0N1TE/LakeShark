// SPDX-License-Identifier: MIT
// Compatibility header: provides std::span for toolchains that do not ship it
// (e.g. xtensa-esp32-elf-gcc 8.x which targets C++20 via -std=gnu++2a but
// lacks the <span> header).  On toolchains that DO have <span> (GCC 10+,
// Clang 7+, MSVC 19.26+) the native header is used transparently.
//
// We guard on __cpp_lib_span (the C++20 feature-test macro) rather than
// __has_include(<span>) because some toolchains (e.g. older ESP32 Arduino
// framework builds) ship a partial <span> that __has_include finds but that
// lacks the contiguous-range/container constructors.  __cpp_lib_span is only
// defined (in <version> or <span>) when the library implementation is fully
// conformant.
#pragma once

// Load feature-test macros without pulling in all of <span>.
#if defined(__has_include) && __has_include(<version>)
#include <version>
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#include <span>
#else

// ---------------------------------------------------------------------------
// Minimal std::span<T> polyfill — covers all operations used in this codebase
// ---------------------------------------------------------------------------
#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace std {

inline constexpr size_t dynamic_extent = static_cast<size_t>(-1);

template <typename T, size_t Extent = dynamic_extent>
class span {
   public:
    using element_type = T;
    using value_type = typename remove_cv<T>::type;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using reverse_iterator = std::reverse_iterator<iterator>;

    // ---- constructors -------------------------------------------------------

    constexpr span() noexcept : data_(nullptr), size_(0) {}

    constexpr span(T* ptr, size_type count) noexcept
        : data_(ptr), size_(count) {}

    constexpr span(T* first, T* last) noexcept
        : data_(first), size_(static_cast<size_type>(last - first)) {}

    template <size_t N>
    constexpr span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}

    template <typename V, size_t N>
    constexpr span(std::array<V, N>& arr) noexcept
        : data_(arr.data()), size_(N) {}

    template <typename V, size_t N>
    constexpr span(const std::array<V, N>& arr) noexcept
        : data_(arr.data()), size_(N) {}

    // Contiguous-container constructor (matches std::vector, std::string, etc.)
    // Enabled only when Container has .data()/.size() and is not a raw array.
    template <typename Container,
              typename = std::enable_if_t<
                  !std::is_array<Container>::value &&
                  std::is_convertible<
                      decltype(std::declval<Container&>().data()), T*>::value>>
    constexpr span(Container& c) noexcept
        : data_(c.data()), size_(static_cast<size_type>(c.size())) {}

    template <
        typename Container,
        typename = std::enable_if_t<
            !std::is_array<Container>::value &&
            std::is_convertible<
                decltype(std::declval<const Container&>().data()), T*>::value>>
    constexpr span(const Container& c) noexcept
        : data_(c.data()), size_(static_cast<size_type>(c.size())) {}

    constexpr span(const span&) noexcept = default;
    constexpr span& operator=(const span&) noexcept = default;

    // ---- observers ----------------------------------------------------------

    constexpr pointer data() const noexcept { return data_; }

    constexpr size_type size() const noexcept { return size_; }

    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr size_type size_bytes() const noexcept {
        return size_ * sizeof(element_type);
    }

    // ---- element access -----------------------------------------------------

    constexpr reference operator[](size_type idx) const noexcept {
        return data_[idx];
    }

    constexpr reference front() const noexcept { return data_[0]; }

    constexpr reference back() const noexcept { return data_[size_ - 1]; }

    // ---- iterators ----------------------------------------------------------

    constexpr iterator begin() const noexcept { return data_; }

    constexpr iterator end() const noexcept { return data_ + size_; }

    constexpr reverse_iterator rbegin() const noexcept {
        return reverse_iterator(end());
    }

    constexpr reverse_iterator rend() const noexcept {
        return reverse_iterator(begin());
    }

    // ---- sub-spans ----------------------------------------------------------

    constexpr span subspan(size_type offset,
                           size_type count = dynamic_extent) const noexcept {
        const size_type len =
            (count == dynamic_extent) ? (size_ - offset) : count;
        return span(data_ + offset, len);
    }

    constexpr span first(size_type count) const noexcept {
        return span(data_, count);
    }

    constexpr span last(size_type count) const noexcept {
        return span(data_ + (size_ - count), count);
    }

   private:
    T* data_;
    size_type size_;
};

// Deduction guides (C++17, but harmless on C++20)
template <typename T, size_t N>
span(T (&)[N]) -> span<T, N>;

template <typename T, size_t N>
span(std::array<T, N>&) -> span<T, N>;

template <typename T, size_t N>
span(const std::array<T, N>&) -> span<const T, N>;

}  // namespace std

#endif  // __has_include(<span>)
