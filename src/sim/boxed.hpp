#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace ts2::sim {

// An optional value kept on the heap, which takes a pointer's room while it is empty. It is for
// large things only a few objects have: a Sim's walk is 592 bytes, and every chair and lamp would
// carry room for one. It reads and writes as a std::optional does, and a copy copies the value.
template <class T>
class Boxed {
public:
    Boxed() = default;
    Boxed(std::nullopt_t) {}
    Boxed(T value) : value_(std::make_unique<T>(std::move(value))) {}
    Boxed(const Boxed& other) : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    Boxed(Boxed&&) noexcept = default;
    Boxed& operator=(const Boxed& other) {
        if (this != &other) {
            value_ = other.value_ ? std::make_unique<T>(*other.value_) : nullptr;
        }
        return *this;
    }
    Boxed& operator=(Boxed&&) noexcept = default;
    Boxed& operator=(std::nullopt_t) {
        value_.reset();
        return *this;
    }
    Boxed& operator=(T value) {
        if (value_) {
            *value_ = std::move(value);
        } else {
            value_ = std::make_unique<T>(std::move(value));
        }
        return *this;
    }

    template <class... Args>
    T& emplace(Args&&... args) {
        value_ = std::make_unique<T>(std::forward<Args>(args)...);
        return *value_;
    }
    void reset() { value_.reset(); }

    [[nodiscard]] bool has_value() const { return value_ != nullptr; }
    explicit operator bool() const { return value_ != nullptr; }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    T* operator->() { return value_.get(); }
    const T* operator->() const { return value_.get(); }
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    template <class U>
    [[nodiscard]] T value_or(U&& fallback) const {
        return value_ ? *value_ : static_cast<T>(std::forward<U>(fallback));
    }

    friend bool operator==(const Boxed& a, std::nullopt_t) { return !a.value_; }
    friend bool operator!=(const Boxed& a, std::nullopt_t) { return a.value_ != nullptr; }

private:
    std::unique_ptr<T> value_;
};

// A std::array of N values that takes a pointer's room until it is written. Until then it reads as
// zeros. It is for a Sim's 256 person data fields on objects that are not Sims. The non-const
// operator[] can't tell a read from a write and makes the array too, so code that only looks
// should hold a const reference.
template <class T, std::size_t N>
class LazyArray {
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    LazyArray() = default;
    LazyArray(const LazyArray& other) : values_(other.values_ ? std::make_unique<std::array<T, N>>(*other.values_) : nullptr) {}
    LazyArray(LazyArray&&) noexcept = default;
    LazyArray& operator=(const LazyArray& other) {
        if (this != &other) {
            values_ = other.values_ ? std::make_unique<std::array<T, N>>(*other.values_) : nullptr;
        }
        return *this;
    }
    LazyArray& operator=(LazyArray&&) noexcept = default;

    static constexpr std::size_t size() { return N; }
    [[nodiscard]] bool allocated() const { return values_ != nullptr; }

    const T& operator[](std::size_t i) const { return values_ ? (*values_)[i] : zeros()[i]; }
    T& operator[](std::size_t i) { return (*made())[i]; }
    iterator begin() { return made()->data(); }
    iterator end() { return made()->data() + N; }
    const_iterator begin() const { return values_ ? values_->data() : zeros().data(); }
    const_iterator end() const { return begin() + N; }
    void fill(const T& value) { made()->fill(value); }

private:
    static const std::array<T, N>& zeros() {
        static const std::array<T, N> none{};
        return none;
    }
    std::array<T, N>* made() {
        if (!values_) {
            values_ = std::make_unique<std::array<T, N>>();
        }
        return values_.get();
    }

    std::unique_ptr<std::array<T, N>> values_;
};

} // namespace ts2::sim
