#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace ts2::sim {

// The game's sort for the wants it offers. Equal elements end in the game's order, so do not swap this
// for std::sort or std::stable_sort. It is an introsort: median-of-three splits down to 16 elements,
// heap sort past a depth of 2 * log2(size), then one insertion sort over the whole range. Its loops
// don't check the range's ends, so `less` must be a strict weak ordering (no NaN keys).
namespace game_sort {

constexpr std::ptrdiff_t kInsertionLimit = 16;

template <class It, class Less>
It median(It a, It b, It c, Less& less) {
    if (!less(*a, *b)) {
        if (less(*a, *c)) {
            return a;
        }
        if (less(*b, *c)) {
            return c;
        }
    } else if (!less(*b, *c)) {
        return less(*a, *c) ? c : a;
    }
    return b;
}

// Moves the elements around the pivot value, swapping from both ends. Returns where the right side starts.
template <class It, class T, class Less>
It partition(It first, It last, const T& pivot, Less& less) {
    for (;;) {
        while (less(*first, pivot)) {
            ++first;
        }
        --last;
        while (less(pivot, *last)) {
            --last;
        }
        if (!(first < last)) {
            return first;
        }
        std::iter_swap(first, last);
        ++first;
    }
}

// Puts `value` at `hole` after moving it up past the parents that go after it, but not above `top`.
template <class It, class T, class Less>
void pushHeap(It first, std::ptrdiff_t hole, std::ptrdiff_t top, T value, Less& less) {
    while (top < hole) {
        const std::ptrdiff_t parent = (hole - 1) / 2;
        if (!less(first[parent], value)) {
            break;
        }
        first[hole] = std::move(first[parent]);
        hole = parent;
    }
    first[hole] = std::move(value);
}

// Moves the hole at `hole` down to a leaf along the children that go first, then places `value` back up from there.
template <class It, class T, class Less>
void adjustHeap(It first, std::ptrdiff_t hole, std::ptrdiff_t size, T value, Less& less) {
    const std::ptrdiff_t top = hole;
    std::ptrdiff_t child = 2 * hole + 2;
    while (child < size) {
        if (less(first[child], first[child - 1])) {
            --child;
        }
        first[hole] = std::move(first[child]);
        hole = child;
        child = 2 * hole + 2;
    }
    if (child == size) {
        first[hole] = std::move(first[size - 1]);
        hole = size - 1;
    }
    pushHeap(first, hole, top, std::move(value), less);
}

template <class It, class Less>
void heapSort(It first, It last, Less& less) {
    using T = typename std::iterator_traits<It>::value_type;
    const std::ptrdiff_t size = last - first;
    if (size > 1) {
        for (std::ptrdiff_t hole = (size - 2) / 2;; --hole) {
            T value = std::move(first[hole]);
            adjustHeap(first, hole, size, std::move(value), less);
            if (hole == 0) {
                break;
            }
        }
    }
    for (std::ptrdiff_t count = size; count > 1; --count) {
        T value = std::move(first[count - 1]);
        first[count - 1] = std::move(first[0]);
        adjustHeap(first, 0, count - 1, std::move(value), less);
    }
}

template <class It, class Less>
void introsort(It first, It last, std::ptrdiff_t depth, Less& less) {
    using T = typename std::iterator_traits<It>::value_type;
    while (last - first > kInsertionLimit) {
        if (depth == 0) {
            heapSort(first, last, less);
            return;
        }
        --depth;
        const T pivot = *median(first, first + (last - first) / 2, last - 1, less);
        const It cut = partition(first, last, pivot, less);
        introsort(cut, last, depth, less);
        last = cut;
    }
}

// Moves the element at `last` down past the ones that go after it. Some element before it must not go after it.
template <class It, class Less>
void unguardedInsert(It last, Less& less) {
    using T = typename std::iterator_traits<It>::value_type;
    T value = std::move(*last);
    for (It before = last - 1; less(value, *before); --before) {
        *last = std::move(*before);
        last = before;
    }
    *last = std::move(value);
}

template <class It, class Less>
void insertionSort(It first, It last, Less& less) {
    using T = typename std::iterator_traits<It>::value_type;
    if (first == last) {
        return;
    }
    for (It next = first + 1; next != last; ++next) {
        if (less(*next, *first)) {
            T value = std::move(*next);
            std::move_backward(first, next, next + 1);
            *first = std::move(value);
        } else {
            unguardedInsert(next, less);
        }
    }
}

}  // namespace game_sort

template <class It, class Less>
void gameSort(It first, It last, Less less) {
    if (first == last) {
        return;
    }
    const std::ptrdiff_t size = last - first;
    std::ptrdiff_t depth = 0;
    for (std::ptrdiff_t rest = size; rest != 1; rest >>= 1) {
        ++depth;
    }
    game_sort::introsort(first, last, depth * 2, less);
    if (size > game_sort::kInsertionLimit) {
        game_sort::insertionSort(first, first + game_sort::kInsertionLimit, less);
        for (It next = first + game_sort::kInsertionLimit; next != last; ++next) {
            game_sort::unguardedInsert(next, less);
        }
    } else {
        game_sort::insertionSort(first, last, less);
    }
}

// Sorts the way the game sorts the choices of an autonomous Sim. `order(a, b)` returns more than 0 when a goes after
// b, less than 0 when it goes before and 0 when they are equal. It is the C library's quicksort of that time, so the
// order of equal elements is the game's, and a range of 8 or fewer is sorted by repeatedly moving the last-going
// element to the end.
namespace game_qsort {

template <class It, class Order>
void shortSort(It lo, It hi, Order& order) {
    while (lo < hi) {
        It latest = lo;
        for (It next = lo + 1; next <= hi; ++next) {
            if (order(*next, *latest) > 0) {
                latest = next;
            }
        }
        if (latest != hi) {
            std::iter_swap(latest, hi);
        }
        --hi;
    }
}

}  // namespace game_qsort

template <class It, class Order>
void gameQsort(It first, It last, Order order) {
    using game_qsort::shortSort;
    if (last - first < 2) {
        return;
    }
    // The larger piece is saved and the smaller one is worked on first, so the saved pieces never pass the base 2
    // logarithm of the size and 64 is far more than needed.
    It savedLo[64];
    It savedHi[64];
    int saved = 0;
    It lo = first;
    It hi = last - 1;
    for (;;) {
        const std::ptrdiff_t size = hi - lo + 1;
        bool more = false;
        if (size <= 8) {
            shortSort(lo, hi, order);
        } else {
            It mid = lo + size / 2;
            if (order(*lo, *mid) > 0) {
                std::iter_swap(lo, mid);
            }
            if (order(*lo, *hi) > 0) {
                std::iter_swap(lo, hi);
            }
            if (order(*mid, *hi) > 0) {
                std::iter_swap(mid, hi);
            }
            It below = lo;
            It above = hi;
            for (;;) {
                if (mid > below) {
                    do {
                        ++below;
                    } while (below < mid && order(*below, *mid) <= 0);
                }
                if (mid <= below) {
                    do {
                        ++below;
                    } while (below <= hi && order(*below, *mid) <= 0);
                }
                do {
                    --above;
                } while (above > mid && order(*above, *mid) > 0);
                if (above < below) {
                    break;
                }
                std::iter_swap(below, above);
                if (mid == above) {
                    mid = below;
                }
            }
            ++above;
            if (mid < above) {
                do {
                    --above;
                } while (above > mid && order(*above, *mid) == 0);
            }
            if (mid >= above) {
                do {
                    --above;
                } while (above > lo && order(*above, *mid) == 0);
            }
            if (above - lo >= hi - below) {
                if (lo < above) {
                    savedLo[saved] = lo;
                    savedHi[saved] = above;
                    ++saved;
                }
                if (below < hi) {
                    lo = below;
                    more = true;
                }
            } else {
                if (below < hi) {
                    savedLo[saved] = below;
                    savedHi[saved] = hi;
                    ++saved;
                }
                if (lo < above) {
                    hi = above;
                    more = true;
                }
            }
        }
        if (!more) {
            if (saved == 0) {
                return;
            }
            --saved;
            lo = savedLo[saved];
            hi = savedHi[saved];
        }
    }
}

}  // namespace ts2::sim
